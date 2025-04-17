/*
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
 * audio buffer sink
 */

#include "audio.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "internal.h"
#include "formats.h"

#include "libavutil/audio_fifo.h"
#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#define INPUT_ON       1    /**< input is active */
#define INPUT_EOF      2    /**< input has reached EOF (may still be active) */

typedef struct ABufSinkPriv {
    const AVClass *class;

    int nb_inputs;                  /**< number of inputs */
    float *input_scale;             /**< scale for each input */
    uint8_t *input_state;           /**< current state of each input */

    int64_t next_pts;               /**< calculated pts for next output frame */
    int64_t output_duration;        /**< last output frame duration to determin to take a new frame from input or not. */

    int planar;
    int sample_rate;                /**< sample rate */
    AVChannelLayout ch_layout;      /**< channel layout */
    enum AVSampleFormat sample_fmt; /**< sample format */

    AVAudioFifo **fifos;            /**< audio fifo for each input */

    int (*on_event_cb)(void *udata, int evt, int64_t args);
    void *on_event_cb_udata;
} ABufSinkPriv;

#define OFFSET(x) offsetof(ABufSinkPriv, x)
#define A AV_OPT_FLAG_AUDIO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM
#define T AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption abufsink_options[] = {
    { "inputs", "Number of inputs.",
            OFFSET(nb_inputs), AV_OPT_TYPE_INT, {.i64 = 1}, 1, INT16_MAX, A|F },
    { "sample_rate", "sample_rate",
            OFFSET(sample_rate), AV_OPT_TYPE_INT, {.i64 = 0}, -1, INT32_MAX, A|F },
    { "ch_layout", "ch_layout",
            OFFSET(ch_layout), AV_OPT_TYPE_CHLAYOUT, {.str = NULL}, 0, 0, A|F },
    { "sample_fmt", "sample_fmt",
            OFFSET(sample_fmt), AV_OPT_TYPE_SAMPLE_FMT, {.i64 = AV_SAMPLE_FMT_NONE}, -1, INT_MAX, A|F },
    { NULL }
};

AVFILTER_DEFINE_CLASS(abufsink);

static void av_abufsink_set_event_cb(AVFilterContext *ctx,
    int (*on_event_cb)(void *udata, int evt, int64_t args), void *udata)
{
    ABufSinkPriv *s = ctx->priv;

    s->on_event_cb = on_event_cb;
    s->on_event_cb_udata = udata;

    ff_filter_set_ready(ctx, 100);
}

/**
 * Update the scaling factors to apply to each input during mixing.
 *
 * This balances the full volume range between active inputs and handles
 * volume transitions when EOF is encountered on an input but mixing continues
 * with the remaining inputs.
 */
static void calculate_scales(ABufSinkPriv *s)
{
    int activate_inputs = 0;
    int i;

    for (i = 0; i < s->nb_inputs; i++)
        if (s->input_state[i] & INPUT_ON)
            activate_inputs++;

    for (i = 0; i < s->nb_inputs; i++) {
        if (s->input_state[i] & INPUT_ON) {
            s->input_scale[i] = 1.0 / activate_inputs;
        }
    }
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

static int input_frame_wanted(ABufSinkPriv *s, int pad)
{
    return !s->fifos[pad] ||
           av_rescale_q(av_audio_fifo_size(s->fifos[pad]), av_make_q(1, s->sample_rate), AV_TIME_BASE_Q) <= s->output_duration;
}

static void request_frame(AVFilterContext *ctx, int pad)
{
    ABufSinkPriv *s = ctx->priv;
    AVFilterLink *link = ctx->inputs[pad];
    int64_t pts;
    int ret;

    ff_inlink_acknowledge_status(link, &ret, &pts);
    if (ret < 0) {
        s->input_state[pad] |= INPUT_EOF;
        av_log(ctx, AV_LOG_INFO, "%s inputs[%d] reached EOF.\n", ctx->name, pad);
        return;
    }

    if (s->on_event_cb)
        ff_inlink_request_frame(link);
    else {
        if (!ff_inlink_queued_frames(link)) {
            ff_inlink_set_status(link, AVERROR_EOF);
        } else {
            ff_filter_set_ready(ctx, 100);
        }
    }
}

static int output_frame(AVFilterContext *ctx)
{
    AVFilterLink *inlink = NULL;
    ABufSinkPriv *s = ctx->priv;
    AVFrame *out_buf, *in_buf;
    int  i, ret, ns, nb_samples = INT_MAX;

    for (i = 0; i < s->nb_inputs; i++) {
        if (s->input_state[i] & INPUT_ON) {
            if (!inlink)
                inlink = ctx->inputs[i];
            ns = av_audio_fifo_size(s->fifos[i]);
            nb_samples = FFMIN(nb_samples, ns);
        }
    }
    if (nb_samples == INT_MAX || nb_samples == 0)
        return 0;

    calculate_scales(s);

    out_buf = ff_get_audio_buffer(inlink, nb_samples);
    if (!out_buf)
        return AVERROR(ENOMEM);

    in_buf = ff_get_audio_buffer(inlink, nb_samples);
    if (!in_buf) {
        av_frame_free(&out_buf);
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < s->nb_inputs; i++) {
        if (s->input_state[i] & INPUT_ON) {
            int planes, plane_size, p;

            ret = av_audio_fifo_read(s->fifos[i], (void **)in_buf->extended_data, nb_samples);
            if (ret < 0) {
                av_frame_free(&out_buf);
                av_frame_free(&in_buf);
                return ret;
            }

            planes = s->planar ? in_buf->ch_layout.nb_channels : 1;
            plane_size = nb_samples * (s->planar ? 1 : in_buf->ch_layout.nb_channels);
            plane_size = FFALIGN(plane_size, 16);

            if (out_buf->format == AV_SAMPLE_FMT_S16 ||
                out_buf->format == AV_SAMPLE_FMT_S16P) {
                for (p = 0; p < planes; p++) {
                    vector_fmac_scalar_c((int16_t *)out_buf->extended_data[p],
                                         (int16_t *) in_buf->extended_data[p],
                                         s->input_scale[i] * INT16_MAX, plane_size);
                }
            } else {
                av_log(ctx, AV_LOG_ERROR, "%s: Unsupported sample format.\n", ctx->name);
                return 0;
            }

            if ((s->input_state[i] & INPUT_EOF) && av_audio_fifo_size(s->fifos[i]) == 0) {
                s->input_state[i] &= ~INPUT_ON;
                av_audio_fifo_free(s->fifos[i]);
                s->fifos[i] = NULL;
            }
        }
    }

    av_frame_free(&in_buf);
    if (s->next_pts == AV_NOPTS_VALUE)
        s->next_pts = 0;

    s->output_duration = av_rescale_q(out_buf->nb_samples, av_make_q(1, s->sample_rate),
                                      AV_TIME_BASE_Q);
    out_buf->pts = s->next_pts;
    s->next_pts += s->output_duration;
    out_buf->duration = s->output_duration;

    if (s->on_event_cb)
        s->on_event_cb(s->on_event_cb_udata, 0, (intptr_t)out_buf);

    av_frame_free(&out_buf);

    return 0;
}

static int abufsink_activate(AVFilterContext *ctx)
{
    ABufSinkPriv *s = ctx->priv;
    AVFrame *frame = NULL;
    AVFilterLink *link;
    int i, ret = 0;
    int64_t pts;

    for (i = 0; i < s->nb_inputs; i++) {
        link = ctx->inputs[i];

        if (ff_inlink_check_available_frame(link)) {
            ret = ff_inlink_consume_frame(link, &frame);
            if (ret < 0) {
                s->input_state[i] |= INPUT_EOF;
                request_frame(ctx, i);
                continue;
            }

            s->input_state[i] = INPUT_ON;
            s->planar = av_sample_fmt_is_planar(link->format);
            s->sample_rate = link->sample_rate;
            s->sample_fmt = link->format;

            if (av_channel_layout_compare(&s->ch_layout, &link->ch_layout))
                av_channel_layout_copy(&s->ch_layout, &link->ch_layout);

            if (!s->fifos[i]) {
                s->fifos[i] = av_audio_fifo_alloc(s->sample_fmt, s->ch_layout.nb_channels, 1024);
                if (!s->fifos[i])
                    return AVERROR(ENOMEM);
            }

            ret = av_audio_fifo_write(s->fifos[i], (void **)frame->extended_data, frame->nb_samples);
            av_frame_free(&frame);
            if (ret < 0)
                return ret;
        }

        ret = output_frame(ctx);
        if (ret < 0)
            return ret;

        if (input_frame_wanted(s, i))
            request_frame(ctx, i);
    }

    return 0;
}

static int abufsink_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                  char *res, int res_len, int flags)
{
    ABufSinkPriv *sink = ctx->priv;
    int ret;

    if (!strcmp(cmd, "link")) {
        int (*on_event_cb)(void *udata, int evt, int64_t args);
        void *udata;

        if (!args)
            return AVERROR(EINVAL);

        if (sscanf(args, "%p %p", &on_event_cb, &udata) != 2)
            return AVERROR(EINVAL);

        if (!sink->on_event_cb)
            av_abufsink_set_event_cb(ctx, on_event_cb, udata);
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        if (sink->on_event_cb)
            sink->on_event_cb(sink->on_event_cb_udata, -1, 0);

        av_abufsink_set_event_cb(ctx, NULL, NULL);
        return 0;
    }

    return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
}

static int abufsink_query_formats(const AVFilterContext *ctx,
                                AVFilterFormatsConfig **cfg_in,
                                AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    ABufSinkPriv *sink = ctx->priv;
    AVChannelLayout layout;
    int ret;

    if (sink->sample_fmt != AV_SAMPLE_FMT_NONE) {
        ret = ff_set_common_formats2(ctx, cfg_in, cfg_out, ff_make_formats_list_singleton(sink->sample_fmt));
        if (ret < 0)
            return ret;
    } else {
        const enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_NONE};
        ret = ff_set_common_formats2(ctx, cfg_in, cfg_out, ff_make_format_list(sample_fmts));
        if (ret < 0)
            return ret;
    }

    if (sink->sample_rate) {
        int sample_rates[] = { sink->sample_rate, -1 };
        ret = ff_set_common_samplerates2(ctx, cfg_in, cfg_out, ff_make_format_list(sample_rates));
        if (ret < 0)
            return ret;
    } else {
        ret = ff_set_common_samplerates2(ctx, cfg_in, cfg_out, ff_all_samplerates());
        if (ret < 0)
            return ret;
    }

    if (sink->ch_layout.nb_channels) {
        const AVChannelLayout layout_list[] = { sink->ch_layout, { 0 } };
        return ff_set_common_channel_layouts2(ctx, cfg_in, cfg_out, ff_make_channel_layout_list(layout_list));
    } else {
        return ff_set_common_channel_layouts2(ctx, cfg_in, cfg_out, ff_all_channel_counts());
    }
}

static int abufsink_init(AVFilterContext *ctx)
{
    ABufSinkPriv *s = ctx->priv;
    int i, ret;

    for (i = 0; i < s->nb_inputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("input%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        if ((ret = ff_append_inpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    s->fifos = av_calloc(s->nb_inputs, sizeof(*s->fifos));
    if (!s->fifos)
        goto err;

    s->input_state = av_malloc(s->nb_inputs);
    if (!s->input_state)
        goto err;
    memset(s->input_state, INPUT_EOF, s->nb_inputs);

    s->input_scale = av_calloc(s->nb_inputs, sizeof(*s->input_scale));
    if (!s->input_state)
        goto err;

    s->next_pts = AV_NOPTS_VALUE;

    return 0;

err:
    av_freep(&s->fifos);
    av_freep(&s->input_state);
    return AVERROR(ENOMEM);
}

static void abufsink_uninit(AVFilterContext *ctx)
{
    ABufSinkPriv *s = ctx->priv;
    int i;

    if (s->fifos) {
        for (i = 0; i < s->nb_inputs; i++)
            av_audio_fifo_free(s->fifos[i]);
        av_freep(&s->fifos);
    }
    av_freep(&s->input_state);
    av_freep(&s->input_scale);
}

const AVFilter ff_asink_abufsink = {
    .name            = "abufsink",
    .description     = NULL_IF_CONFIG_SMALL("audio buffer sink(only pcm)"),
    .priv_size       = sizeof(ABufSinkPriv),
    .priv_class      = &abufsink_class,
    .init            = abufsink_init,
    .uninit          = abufsink_uninit,
    FILTER_QUERY_FUNC2(abufsink_query_formats),
    .activate        = abufsink_activate,
    .process_command = abufsink_process_command,
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS,
};