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

enum MixInputState {
    INPUT_ON  = 1,    /**< input is active */
    INPUT_EOF = 2,    /**< input has reached EOF (may still be active) */
};

#define DURATION_LONGEST  0
#define DURATION_SHORTEST 1
#define DURATION_FIRST    2

/* FIXME: use directly links fifo */

typedef struct MixInput {
    enum MixInputState state; /**< current state of each input */
    float scale;         /**< mixing scale factor for each input */
    float weight;             /**< custom weight for every input */
    float scale_norm;          /**< normalization factor for every input */
    AVFilterContext *ctx;    /**< filter context for each input */

    int (*on_event_cb)(void *udata, int evt, int64_t args);
    void *on_event_cb_udata;

    AResampleContext resample; /**< resampler context */
    AVAudioFifo **fifos;       /**< audio fifo for each output */
} MixInput;

typedef struct MixOutput {
    int64_t next_pts;          /**< next pts to output */
} MixOutput;

typedef struct MixContext {
    const AVClass *class;       /**< class for AVOptions */
    AVFloatDSPContext *fdsp;

    int active_inputs;          /**< number of input currently active */
    int duration_mode;          /**< mode for determining duration */
    float dropout_transition;   /**< transition time when an input drops out */
    int normalize;              /**< if inputs are scaled */

    int sample_rate;            /**< sample rate */
    AVChannelLayout ch_layout;  /**< channel layout */
    enum AVSampleFormat sample_fmt;  /**< sample format */

    float weight_sum;           /**< sum of custom weight for every input */
    MixInput **inputs;           /**< per-input data */
    int nb_inputs;              /**< number of inputs */
    int nb_allocated_inputs;    /**< number of allocated inputs */

    int nb_outputs;             /**< number of outputs */
    MixOutput *outputs;         /**< per-output data */
    int period_ms;              /**< period time in ms for each input */

    pthread_mutex_t mutex;     /**< mutex for thread safety */
} MixContext;

#define OFFSET(x) offsetof(MixContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM
#define T AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption amix_options[] = {
    { "outputs", "Number of outputs.",
            OFFSET(nb_outputs), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, INT16_MAX, A|F },
    { "duration", "How to determine the end-of-stream.",
            OFFSET(duration_mode), AV_OPT_TYPE_INT, { .i64 = DURATION_LONGEST }, 0,  2, A|F, .unit = "duration" },
        { "longest",  "Duration of longest input.",  0, AV_OPT_TYPE_CONST, { .i64 = DURATION_LONGEST  }, 0, 0, A|F, .unit = "duration" },
        { "shortest", "Duration of shortest input.", 0, AV_OPT_TYPE_CONST, { .i64 = DURATION_SHORTEST }, 0, 0, A|F, .unit = "duration" },
        { "first",    "Duration of first input.",    0, AV_OPT_TYPE_CONST, { .i64 = DURATION_FIRST    }, 0, 0, A|F, .unit = "duration" },
    { "dropout_transition", "Transition time, in seconds, for volume "
                            "renormalization when an input stream ends.",
            OFFSET(dropout_transition), AV_OPT_TYPE_FLOAT, { .dbl = 2.0 }, 0, INT_MAX, A|F },
    { "normalize", "Scale inputs",
            OFFSET(normalize), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, A|F|T },
    { "period_ms", "Set period time in ms for each input.",
            OFFSET(period_ms), AV_OPT_TYPE_INT, {.i64=20}, 0, INT_MAX, A|F },

    { NULL }
};

AVFILTER_DEFINE_CLASS(amix);

static int amix_buffersrc_open(MixInput **input, AVFilterContext *ctx,
                               int (*on_event_cb)(void *udata, int evt, int64_t args),
                               void *on_event_cb_udata)
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
        return AVERROR(ENOMEM);

    in->weight = 1.0f;
    in->state = INPUT_ON;
    in->ctx = ctx;
    in->on_event_cb = on_event_cb;
    in->on_event_cb_udata = on_event_cb_udata;
    ff_resample_init(&in->resample);

    pthread_mutex_lock(&s->mutex);
    s->inputs[s->nb_inputs] = in;
    s->nb_inputs++;
    in->fifos = av_mallocz(s->nb_outputs * sizeof(*in->fifos));
    if (!in->fifos) {
        pthread_mutex_unlock(&s->mutex);
        return AVERROR(ENOMEM);
    }
    pthread_mutex_unlock(&s->mutex);

    for (i = 0; i < s->nb_outputs; i++) {
        FilterLinkInternal *li = ff_link_internal(ctx->outputs[i]);
        li->frame_wanted_out = 1;
    }
    ff_filter_set_ready(ctx, 100);

    *input = in;
    return 0;
}

static int amix_buffersrc_close(MixInput **pin)
{
    MixContext *s;
    MixInput *in;
    int i, ret;

    if (!pin || !*pin)
        return 0;

    in = *pin;
    s = in->ctx->priv;

    in->state = INPUT_EOF;

    pthread_mutex_lock(&s->mutex);
    for (i = 0; i < s->nb_inputs; i++) {
        if (s->inputs[i] == in)
            break;
    }
    if (i == s->nb_inputs) {
        av_log(in->ctx, AV_LOG_ERROR, "input not found\n");
        pthread_mutex_unlock(&s->mutex);
        return AVERROR(EINVAL);
    }
    pthread_mutex_unlock(&s->mutex);

    *pin = NULL;

    return 0;
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

    for (i = 0; i < s->nb_inputs; i++)
        if (s->inputs[i]->state & INPUT_ON)
            weight_sum += FFABS(s->inputs[i]->weight);

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

        if (in->state == INPUT_EOF)
            continue;

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
                                         in->scale * INT16_MAX, plane_size);
                }
            } else if (out_buf->format == AV_SAMPLE_FMT_FLT ||
                       out_buf->format == AV_SAMPLE_FMT_FLTP) {
                for (p = 0; p < planes; p++) {
                    s->fdsp->vector_fmac_scalar((float *)out_buf->extended_data[p],
                                                (float *) in_buf->extended_data[p],
                                                in->scale, plane_size);
                }
            } else {
                for (p = 0; p < planes; p++) {
                    s->fdsp->vector_dmac_scalar((double *)out_buf->extended_data[p],
                                                (double *) in_buf->extended_data[p],
                                                in->scale, plane_size);
                }
            }
        }
    }

    av_frame_free(&in_buf);
    if (s->outputs[index].next_pts == AV_NOPTS_VALUE)
        s->outputs[index].next_pts = 0;

    out_buf->pts = s->outputs[index].next_pts;
    out_buf->duration = av_rescale_q(out_buf->nb_samples, av_make_q(1, outlink->sample_rate),
                                     outlink->time_base);
    s->outputs[index].next_pts += nb_samples;

    return ff_filter_frame(outlink, out_buf);
}

static int activate(AVFilterContext *ctx)
{
    MixContext *s = ctx->priv;
    MixInput *in;
    int i, j, ret, is_eof = 0;

    for (i = 0; i < s->nb_outputs; i++) {
        if (!ff_outlink_frame_wanted(ctx->outputs[i])) {
            return 0;
        }
    }

    pthread_mutex_lock(&s->mutex);
    for (i = 0; i < s->nb_inputs; i++) {
        MixInput *in = s->inputs[i];
        AVFrame *src, *dst;

        if (in->state == INPUT_EOF)
            continue;

        src = av_frame_alloc();
        if (!src) {
            pthread_mutex_unlock(&s->mutex);
            return AVERROR(ENOMEM);
        }

        if (in->on_event_cb)
            in->on_event_cb(in->on_event_cb_udata, 0, (intptr_t)src);

        for (j = 0; j < s->nb_outputs; j++) {
            if (!in->fifos[j]) {
                in->fifos[j] = av_audio_fifo_alloc(ctx->outputs[j]->format, ctx->outputs[j]->ch_layout.nb_channels, 1024);
                if (!in->fifos[j]) {
                    pthread_mutex_unlock(&s->mutex);
                    return AVERROR(ENOMEM);
                }
            }

            ret = ff_resample_frame(&in->resample, ctx->outputs[j], src, &dst);
            if (ret <= 0) {
                dst = av_frame_clone(src);
                if (!dst) {
                    av_frame_free(&src);
                    pthread_mutex_unlock(&s->mutex);
                    return AVERROR(ENOMEM);
                }
            }
            ret = av_audio_fifo_write(in->fifos[j], (void **)dst->extended_data, dst->nb_samples);
            if (ret < 0) {
                pthread_mutex_unlock(&s->mutex);
                return ret;
            }
            av_frame_free(&dst);
        }
        av_frame_free(&src);
    }

    calculate_scales(s, 0);

    for (i = 0; i < s->nb_outputs; i++) {
        int nb_samples = 4096;

        for (j = 0; j < s->nb_inputs; j++) {
            in = s->inputs[j];
            if (in->state == INPUT_EOF)
                continue;

            nb_samples = FFMIN(av_audio_fifo_size(in->fifos[i]), nb_samples);
        }

        ret = output_frame(ctx, i, nb_samples);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "outputlink[%d] out_frame failed ret:%d:%s\n", i, ret, av_err2str(ret));
            pthread_mutex_unlock(&s->mutex);
            return ret;
        }
    }

    for (i = 0; i < s->nb_inputs;) {
        in = s->inputs[i];
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

    is_eof = !s->nb_inputs;
    pthread_mutex_unlock(&s->mutex);

    if (is_eof) {
        for (i = 0; i < s->nb_outputs; i++) {
            AVFrame *frame = av_frame_alloc();
            if (!frame)
                return AVERROR(ENOMEM);
            frame->nb_samples = 0;
            frame->format = ctx->outputs[i]->format;
            frame->sample_rate = ctx->outputs[i]->sample_rate;
            av_channel_layout_copy(&frame->ch_layout, &ctx->outputs[i]->ch_layout);
            ff_filter_frame(ctx->outputs[i], frame);
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

    pthread_mutex_init(&s->mutex, NULL);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    int i, j;
    MixContext *s = ctx->priv;

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
    pthread_mutex_destroy(&s->mutex);
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
        int (*on_event_cb)(void *udata, int evt, int64_t args);
        void *udata;

        if (!args || !res)
            return AVERROR(EINVAL);

        if (sscanf(args, "%p %p", &on_event_cb, &udata) != 2)
            return AVERROR(EINVAL);

        ret = amix_buffersrc_open(&in, ctx, on_event_cb, udata);
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

        ret = amix_buffersrc_close(&in);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "amixsrc: error closing input: %s\n", av_err2str(ret));
            return ret;
        }

        return 0;
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
    const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
        AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_DBLP,
        AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_NONE
    };
    MixContext *s = ctx->priv;
    int ret;

    if ((ret = ff_set_common_formats2(ctx, cfg_in, cfg_out, ff_make_format_list(sample_fmts))) < 0)
        return ret;

    if ((ret = ff_set_common_samplerates2(ctx, cfg_in, cfg_out, ff_all_samplerates())) < 0)
        return ret;

    return ff_set_common_channel_layouts2(ctx, cfg_in, cfg_out, ff_all_channel_counts());
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
