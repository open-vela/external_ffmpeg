/*
 * Audio Mix Source Filter
 * Copyright (c) 2012 Justin Ruggles <justin.ruggles@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Audio Mix Source Filter
 *
 * Mixes audio from multiple sources into a single output. The channel layout,
 * sample rate, and sample format will be the same for all inputs and the
 * output.
 */

#include "libavutil/attributes.h"
#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/float_dsp.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"

#include "audio.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"
#include <pthread.h>
#include "aresample.h"
#include "mapping.h"

enum MixInputState {
    INPUT_ON  = 1,    /**< input is active */
    INPUT_EOF = 2,    /**< input has reached EOF (may still be active) */
};

#define DURATION_LONGEST  0
#define DURATION_SHORTEST 1
#define DURATION_FIRST    2

/* FIXME: use directly links fifo */

typedef struct MixInput {
    enum MixInputState state;  /**< current state of each input */
    float scale;               /**< mixing scale factor for each input */
    float weight;              /**< custom weight for every input */
    float scale_norm;          /**< normalization factor for every input */
    float volume;              /**< custom volume for each input */
    AVFilterContext *ctx;      /**< filter context for each input */

    int (*on_event_cb)(void *udata, int evt, int64_t args);
    void *on_event_cb_udata;

    AResampleContext resample; /**< resampler context */
    AVAudioFifo **fifos;       /**< audio fifo for each output */

    int sample_rate;            /**< sample rate */
    AVChannelLayout ch_layout;  /**< channel layout */
    enum AVSampleFormat sample_fmt;  /**< sample format */
} MixInput;

typedef struct MixOutput {
    int64_t next_pts;          /**< next pts to output */
} MixOutput;

typedef struct MixContext {
    const AVClass *class;            /**< class for AVOptions */
    AVFloatDSPContext *fdsp;

    float dropout_transition;        /**< transition time when an input drops out */
    int normalize;                   /**< if inputs are scaled */
    float volume;                    /**< custom stream volume, eg: Music, Ring. */
    float volume_last;               /**< last custom stream volume */

    int sample_rate;                 /**< sample rate */
    AVChannelLayout ch_layout;       /**< channel layout */
    enum AVSampleFormat sample_fmt;  /**< sample format */

    float weight_sum;                /**< sum of custom weight for every input */
    MixInput **inputs;               /**< per-input data */
    int nb_inputs;                   /**< number of inputs */
    int nb_allocated_inputs;         /**< number of allocated inputs */

    char *map_str;
    int *map;                        /**< map from input to output */
    int nb_outputs;                  /**< number of outputs */
    int64_t output_duration;         /**< last output frame duration to determin to take a new frame from input or not. */
    MixOutput *outputs;              /**< per-output data */
} MixContext;

#define OFFSET(x) offsetof(MixContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM
#define T AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption amix_options[] = {
    { "outputs", "Number of outputs.",
            OFFSET(nb_outputs), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, INT16_MAX, A|F },
    { "dropout_transition", "Transition time, in seconds, for volume "
                            "renormalization when an input stream ends.",
            OFFSET(dropout_transition), AV_OPT_TYPE_FLOAT, { .dbl = 2.0 }, 0, INT_MAX, A|F },
    { "normalize", "Scale inputs",
            OFFSET(normalize), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, A|F|T },
    { "map", "input indexes to remap to outputs", OFFSET(map_str),    AV_OPT_TYPE_STRING, {.str=NULL},    .flags = A|F },
    { "map_array", "get map list", OFFSET(map),    AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX,    .flags = A|F },
    { NULL }
};

AVFILTER_DEFINE_CLASS(amix);

/**
 * Clear closed inputs, and rearrange inputs array.
 */
static int clear_inputs(AVFilterContext *ctx)
{
    MixContext *s = ctx->priv;
    MixInput *in;
    int i, j;

    for (i = 0; i < s->nb_inputs;) {
        in = s->inputs[i];

        if (in->state & INPUT_EOF) { /* If all fifos of current input are empty, set INPUT_EOF */
            for (j = 0; j < s->nb_outputs; j++)
                if (in->fifos[j] && av_audio_fifo_size(in->fifos[j]) != 0)
                    break;
            if (j == s->nb_outputs)
                in->state = INPUT_EOF;
        }

        if (in->state != INPUT_EOF) {
            i++;
            continue;
        }

        for (j = 0; j < s->nb_outputs; j++) {
            if (in->fifos[j]) {
                av_audio_fifo_free(in->fifos[j]);
                in->fifos[j] = NULL;
            }
        }

        av_freep(&in->fifos);
        av_freep(&s->inputs[i]);

        for (j = i; j < s->nb_inputs - 1; j++) {
            s->inputs[j] = s->inputs[j + 1];
        }

        s->inputs[s->nb_inputs - 1] = NULL;
        s->nb_inputs--;
    }

    return s->nb_inputs;
}

static int amix_buffersrc_open(MixInput **input, AVFilterContext *ctx,
                               int (*on_event_cb)(void *udata, int evt, int64_t args),
                               void *on_event_cb_udata, int format, int sample_rate, int channels)
{
    MixContext *s = ctx->priv;
    MixInput *in;
    int i, ret;

    if (s->nb_inputs >= s->nb_allocated_inputs) {
        int new_size = s->nb_allocated_inputs ? s->nb_allocated_inputs * 2 : 4;
        s->inputs = av_realloc_array(s->inputs, new_size, sizeof(*s->inputs));
        if (!s->inputs)
            return AVERROR(ENOMEM);
        s->nb_allocated_inputs = new_size;
    }

    in = av_mallocz(sizeof(*in));
    if (!in)
        goto err;

    in->fifos = av_mallocz(s->nb_outputs * sizeof(*in->fifos));
    if (!in->fifos)
        goto err;

    in->on_event_cb_udata = on_event_cb_udata;
    in->on_event_cb = on_event_cb;
    in->state = INPUT_ON;
    in->weight = 1.0f;
    in->volume = 1.0f;
    in->ctx = ctx;

    ff_resample_init(&in->resample);

    in->sample_fmt = format;
    in->sample_rate = sample_rate;
    av_channel_layout_default(&in->ch_layout, channels);

    s->inputs[s->nb_inputs] = in;
    s->nb_inputs++;

    for (i = 0; i < s->nb_outputs; i++) {
        FilterLinkInternal *li;
        if (s->map && s->map[i] == ROUTE_OFF)
            continue;

        li = ff_link_internal(ctx->outputs[i]);
        li->frame_wanted_out = 1;
    }
    ff_filter_set_ready(ctx, 100);

    *input = in;
    return 0;

err:
    if (in) {
        av_freep(&in->fifos);
        av_freep(&in);
    }
    return AVERROR(ENOMEM);
}

static int amix_buffersrc_close(MixInput **pin)
{
    MixContext *s;
    MixInput *in;
    int i, ret;

    if (!pin || !*pin)
        return AVERROR(EINVAL);

    in = *pin;
    s = in->ctx->priv;

    for (i = 0; i < s->nb_inputs; i++) {
        if (s->inputs[i] == in)
            break;
    }
    if (i == s->nb_inputs) {
        av_log(in->ctx, AV_LOG_ERROR, "input not found\n");
        return AVERROR(EINVAL);
    }

    ff_resample_uninit(&in->resample);

    in->state |= INPUT_EOF;

    clear_inputs(in->ctx);

    *pin = NULL;

    return 0;
}

static int set_parameter(MixInput *in, const char *key, const char *value)
{
    if (!in || !key || !value)
        return AVERROR(EINVAL);

    if (!strcmp(key, "volume")) {
        in->volume = strtof(value, NULL);

        av_log(in->ctx, AV_LOG_DEBUG, "set_parameter: %s = %.2f\n", key, in->volume);
        return 0;
    }

    av_log(in->ctx, AV_LOG_ERROR, "set_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

static int get_parameter(AVFilterContext *ctx, MixInput *in, const char *key, char *value, int len)
{
    MixContext *s = ctx->priv;

    if (!key || len <= 0)
        return AVERROR(EINVAL);

    if (!strcmp(key, "volume")) {
        snprintf(value, len, "vol:%f", in->volume);

        av_log(in->ctx, AV_LOG_DEBUG, "get_parameter: %s = %.2f\n", key, in->volume);
        return 0;
    }  else if (!strcmp(key, "get_format")) {
        MixInput *cur;
        int i;

        if (s->nb_inputs == 0)
            return  snprintf(value, len, "fmt=0:rate=0:ch=0");

        for (i = 0; i < s->nb_inputs; i++) {
            cur = s->inputs[i];
            if (cur->state != INPUT_EOF)
                break;
        }

        snprintf(value, len, "fmt=%d:rate=%d:ch=%d", cur->sample_fmt, cur->sample_rate, cur->ch_layout.nb_channels);
        return 0;
    }

    av_log(in->ctx, AV_LOG_ERROR, "get_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

/**
 * Update the scaling factors to apply to each input during mixing.
 *
 * This balances the full volume range between active inputs and handles
 * volume transitions when EOF is encountered on an input but mixing continues
 * with the remaining inputs.
 */
static void calculate_scales(MixContext *s, int nb_samples)
{
    float weight_sum = 0.f;
    int i;

    s->weight_sum = 0.f;

    for (i = 0; i < s->nb_inputs; i++)
        s->weight_sum += FFABS(s->inputs[i]->weight);

    for (i = 0; i < s->nb_inputs; i++) {
        s->inputs[i]->scale_norm = s->weight_sum / FFABS(s->inputs[i]->weight);
        if (s->inputs[i]->state & INPUT_ON)
            weight_sum += FFABS(s->inputs[i]->weight);
    }

    for (i = 0; i < s->nb_inputs; i++) {
        if (s->inputs[i]->state & INPUT_ON) {
            if (s->inputs[i]->scale_norm > weight_sum / FFABS(s->inputs[i]->weight)) {
                s->inputs[i]->scale_norm -= ((s->weight_sum / FFABS(s->inputs[i]->weight)) / s->nb_inputs) *
                                    nb_samples / (s->dropout_transition * s->sample_rate);
                s->inputs[i]->scale_norm = FFMAX(s->inputs[i]->scale_norm, weight_sum / FFABS(s->inputs[i]->weight));
            }
        }
    }

    for (i = 0; i < s->nb_inputs; i++) {
        if (s->inputs[i]->state & INPUT_ON) {
            if (!s->normalize)
                s->inputs[i]->scale = FFABS(s->inputs[i]->weight);
            else
                s->inputs[i]->scale = 1.0f / s->inputs[i]->scale_norm * FFSIGN(s->inputs[i]->weight);
        } else {
            s->inputs[i]->scale = 0.0f;
        }
    }
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    MixContext *s      = ctx->priv;
    int i;
    char buf[64];

    outlink->time_base = (AVRational){ 1, outlink->sample_rate };

    av_channel_layout_describe(&outlink->ch_layout, buf, sizeof(buf));

    av_log(ctx, AV_LOG_VERBOSE,
           "inputs:%d fmt:%s srate:%d cl:%s\n", s->nb_inputs,
           av_get_sample_fmt_name(outlink->format), outlink->sample_rate, buf);

    return 0;
}

static void vector_fmac_scalar_c(int16_t *dst, const int16_t *src, int16_t mul, int len)
{
    int i;
    int32_t accu;

    for (i = 0; i < len; i++) {
        accu = (int32_t)src[i] * mul;
        dst[i] = av_clip_int16(dst[i] + ((accu + 0x4000) >> 15));
    }
}

static int frame_wanted(AVFilterContext *ctx, MixInput *in)
{
    MixContext *s = ctx->priv;
    int j;

    /* If all outputs fifo size are bigger than last output size, then skip current read frame. */
    for (j = 0; j < s->nb_outputs; j++) {
        if (s->map[j] == ROUTE_OFF ||
            (in->fifos[j] &&
             av_rescale_q(av_audio_fifo_size(in->fifos[j]),
                          av_make_q(1, ctx->outputs[j]->sample_rate),
                          AV_TIME_BASE_Q) > s->output_duration))
            continue;
        break;
    }

    return j != s->nb_outputs;
}

/**
 * Read samples from the input FIFOs, mix, and write to the output link.
 */
static int output_frame(AVFilterContext *ctx, int index, int nb_samples)
{
    AVFilterLink *outlink = ctx->outputs[index];
    MixContext *s = ctx->priv;
    AVFrame *out_buf, *in_buf;
    int ns, i, ret;

    out_buf = ff_get_audio_buffer(outlink, nb_samples);
    if (!out_buf)
        return AVERROR(ENOMEM);

    in_buf = ff_get_audio_buffer(outlink, nb_samples);
    if (!in_buf) {
        av_frame_free(&out_buf);
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < s->nb_inputs; i++) {
        MixInput *in = s->inputs[i];
        int planar;

        ret = av_audio_fifo_read(in->fifos[index], (void **)in_buf->extended_data, nb_samples);
        if (ret < 0) {
            av_frame_free(&out_buf);
            av_frame_free(&in_buf);
            return ret;
        }

        planar = av_sample_fmt_is_planar(in_buf->format);
        if (in->state & INPUT_ON) {
            int planes, plane_size, p;

            planes = planar ? in_buf->ch_layout.nb_channels : 1;
            plane_size = nb_samples * (planar ? 1 : in_buf->ch_layout.nb_channels);
            plane_size = FFALIGN(plane_size, 16);

            if (out_buf->format == AV_SAMPLE_FMT_S16 ||
                out_buf->format == AV_SAMPLE_FMT_S16P) {
                for (p = 0; p < planes; p++) {
                    vector_fmac_scalar_c((int16_t *)out_buf->extended_data[p],
                                         (int16_t *) in_buf->extended_data[p],
                                         in->scale * INT16_MAX * in->volume, plane_size);
                }
            } else if (out_buf->format == AV_SAMPLE_FMT_FLT ||
                       out_buf->format == AV_SAMPLE_FMT_FLTP) {
                for (p = 0; p < planes; p++) {
                    s->fdsp->vector_fmac_scalar((float *)out_buf->extended_data[p],
                                                (float *) in_buf->extended_data[p],
                                                in->scale * in->volume, plane_size);
                }
            } else {
                for (p = 0; p < planes; p++) {
                    s->fdsp->vector_dmac_scalar((double *)out_buf->extended_data[p],
                                                (double *) in_buf->extended_data[p],
                                                in->scale * in->volume, plane_size);
                }
            }
        }
    }

    av_frame_free(&in_buf);
    if (s->outputs[index].next_pts == AV_NOPTS_VALUE)
        s->outputs[index].next_pts = 0;

    s->output_duration = av_rescale_q(out_buf->nb_samples, av_make_q(1, outlink->sample_rate),
                                      AV_TIME_BASE_Q);
    out_buf->pts = s->outputs[index].next_pts;
    s->outputs[index].next_pts += s->output_duration;
    out_buf->duration = s->output_duration;

    if (s->volume != s->volume_last) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%f", s->volume);
        av_dict_set(&out_buf->metadata, "volume", tmp, 0);
        s->volume_last = s->volume;
    }

    return ff_filter_frame(outlink, out_buf);
}

static int activate(AVFilterContext *ctx)
{
    MixContext *s = ctx->priv;
    MixInput *in;
    int i, j, ret;

    for (i = 0; i < s->nb_outputs; i++) {
        if (s->map && s->map[i] == ROUTE_ON)
            break;
    }

    if (i == s->nb_outputs)
        return 0;

    for (i = 0; i < s->nb_inputs; i++) {
        AVFrame *src, *dst;
        in = s->inputs[i];

        if (in->state & INPUT_EOF)
            continue;

        if (!frame_wanted(ctx, in))
            continue;

        src = av_frame_alloc();
        if (!src)
            return AVERROR(ENOMEM);

        if (in->on_event_cb)
            in->on_event_cb(in->on_event_cb_udata, 0, (intptr_t)src);

        for (j = 0; j < s->nb_outputs; j++) {
            if (s->map && s->map[j] == ROUTE_OFF)
                continue;

            if (!in->fifos[j]) {
                in->fifos[j] = av_audio_fifo_alloc(ctx->outputs[j]->format, ctx->outputs[j]->ch_layout.nb_channels, 1024);
                if (!in->fifos[j])
                    return AVERROR(ENOMEM);
            }

            ret = ff_resample_frame(&in->resample, ctx->outputs[j], src, &dst);
            if (ret <= 0) {
                dst = av_frame_clone(src);
                if (!dst) {
                    av_frame_free(&src);
                    return AVERROR(ENOMEM);
                }
            }
            ret = av_audio_fifo_write(in->fifos[j], (void **)dst->extended_data, dst->nb_samples);
            if (ret < 0)
                return ret;

            av_frame_free(&dst);
        }
        av_frame_free(&src);
    }

    calculate_scales(s, 0);

    for (i = 0; i < s->nb_outputs; i++) {
        int nb_samples = INT_MAX;

        if (s->map && s->map[i] == ROUTE_OFF)
            continue;

        for (j = 0; j < s->nb_inputs; j++) {
            in = s->inputs[j];

            nb_samples = FFMIN(av_audio_fifo_size(in->fifos[i]), nb_samples);
        }

        if (nb_samples == INT_MAX)
            continue;

        ret = output_frame(ctx, i, nb_samples);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "outputlink[%d] out_frame failed ret:%d:%s\n", i, ret, av_err2str(ret));
            return ret;
        }
    }

    if (!clear_inputs(ctx)) {
        for (i = 0; i < s->nb_outputs; i++) {
            AVFilterLink *outlink = ctx->outputs[i];
            if (s->map && s->map[i] == ROUTE_OFF)
                continue;

            ff_outlink_set_status(outlink, AVERROR_EOF, AV_NOPTS_VALUE);
        }
    }

    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    MixContext *s = ctx->priv;
    int i, ret;

    s->outputs = av_calloc(s->nb_outputs, sizeof(*s->outputs));
    if (!s->outputs)
        return AVERROR(ENOMEM);

    for (i = 0; i < s->nb_outputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type           = AVMEDIA_TYPE_AUDIO;
        pad.name           = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        pad.config_props   = config_output;
        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;

        s->outputs[i].next_pts = AV_NOPTS_VALUE;
    }

    s->fdsp = avpriv_float_dsp_alloc(0);
    if (!s->fdsp)
        return AVERROR(ENOMEM);

    if (s->map_str) {
        ret = avfilter_parse_mapping(s->map_str, &s->map, s->nb_outputs);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    MixContext *s = ctx->priv;
    int i, j;

    for (i = 0; i < s->nb_inputs; i++) {
        ff_resample_uninit(&s->inputs[i]->resample);
        for (j = 0; j < s->nb_outputs; j++) {
            if (s->inputs[i]->fifos[j]) {
                av_audio_fifo_free(s->inputs[i]->fifos[j]);
                s->inputs[i]->fifos[j] = NULL;
            }
        }
        av_freep(&s->inputs[i]->fifos);
        av_freep(&s->inputs[i]);
        av_log(ctx, AV_LOG_WARNING, "input:%d were not closed.\n", i);
    }

    if (s->outputs)
        av_freep(&s->outputs);

    av_freep(&s->inputs);
    av_freep(&s->fdsp);
    av_freep(&s->map);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    MixContext *s = ctx->priv;
    int ret;

    if (!cmd)
        return AVERROR(EINVAL);

    if (!strcmp(cmd, "link")) {
        MixInput *in;
        int format, sample_rate, channels;
        int (*on_event_cb)(void *udata, int evt, int64_t args);
        void *udata;

        if (!args || !res)
            return AVERROR(EINVAL);

        if (sscanf(args, "%p %p fmt=%d:rate=%d:ch=%d", &on_event_cb, &udata, &format, &sample_rate, &channels) != 5)
            return AVERROR(EINVAL);

        ret = amix_buffersrc_open(&in, ctx, on_event_cb, udata, format, sample_rate, channels);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "amixsrc: error opening input: %s\n", av_err2str(ret));
            return ret;
        }

        *(MixInput **)res = in;

        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        MixInput *in = (MixInput *)args;

        if (!in)
            return AVERROR(EINVAL);

        if (in->on_event_cb)
            in->on_event_cb(in->on_event_cb_udata, -1, 0);

        ret = amix_buffersrc_close(&in);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "amixsrc: error closing input: %s\n", av_err2str(ret));
            return ret;
        }

        return 0;
    } else if (!av_strcasecmp(cmd, "map")) {
        int *old_map = NULL;
        int i;

        if (s->map) {
            old_map = av_calloc(s->nb_outputs, sizeof(*old_map));
            if (!old_map)
                return AVERROR(ENOMEM);

            memcpy(old_map, s->map, s->nb_outputs * sizeof(*old_map));
        }

        ret = avfilter_parse_mapping(args, &s->map, s->nb_outputs);
        if (ret < 0) {
            av_freep(&old_map);
            return ret;
        }

        for (i = 0; i < s->nb_outputs && old_map; i++) {
            if (old_map[i] == ROUTE_ON && s->map[i] == ROUTE_OFF &&
                ff_outlink_frame_wanted(ctx->outputs[i])) {
                ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
            }
        }
        av_freep(&old_map);

        if (s->nb_inputs > 0)
            ff_filter_set_ready(ctx, 100);

        return ret;
    } else if(!strcmp(cmd, "volume")) {
        double value;

        if (!args)
            return AVERROR(EINVAL);

        ret = av_expr_parse_and_eval(&value, args, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR,
                "Error when parsing %s volume expression '%s'\n", ctx->name, args);
            return ret;
        }

        s->volume = value;
        av_log(ctx, AV_LOG_INFO, "set volume:%f volume_dB:%f\n", s->volume, 20.0*log10(s->volume));

        return 0;
    } else if (!strcmp(cmd, "set_parameter")){
        MixInput *in;
        char key[32], value[32];

        if (sscanf(args, "%p %31s %31s", &in, key, value) != 3)
            return AVERROR(EINVAL);

        return set_parameter(in, key, value);
    } else if (!strcmp(cmd, "get_parameter")){
        MixInput *in;
        char key[32];

        if (sscanf(args, "%p %31s", &in, key) != 2)
            return AVERROR(EINVAL);

        return get_parameter(ctx, in, key, res, res_len);
    }

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    return 0;
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    int ret, i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(ff_all_formats(AVMEDIA_TYPE_AUDIO), &cfg_out[i]->formats);
        if (ret < 0)
            goto out;

        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(ff_all_samplerates(), &cfg_out[i]->samplerates);
        if (ret < 0)
            goto out;

        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(ff_all_channel_counts(), &cfg_out[i]->channel_layouts);
    }

out:
    return ret;
}

const AVFilter ff_asrc_amixsrc = {
    .name           = "amixsrc",
    .description    = NULL_IF_CONFIG_SMALL("Audio mixing source."),
    .priv_size      = sizeof(MixContext),
    .priv_class     = &amix_class,
    .init           = init,
    .uninit         = uninit,
    .activate       = activate,
    .inputs         = NULL,
    FILTER_QUERY_FUNC2(query_formats),
    .process_command = process_command,
    .flags          = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
