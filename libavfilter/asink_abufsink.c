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

#include "amix.h"
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

    int64_t next_pts;               /**< calculated pts for next output frame */
    int64_t output_duration;        /**< last output frame duration to determin to take a new frame from input or not. */

    int planar;
    int sample_rate;                /**< sample rate */
    AVChannelLayout ch_layout;      /**< channel layout */
    enum AVSampleFormat sample_fmt; /**< sample format */

    AMixContext *mix;               /**< mix module context */
    int frame_size;                 /**< frame size */

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

static void request_frame(AVFilterContext *ctx, int pad)
{
    ABufSinkPriv *s = ctx->priv;
    AVFilterLink *link = ctx->inputs[pad];
    int64_t pts;
    int i, ret;

    ff_inlink_acknowledge_status(link, &ret, &pts);
    if (ret < 0)
        return;

    if (s->on_event_cb && (!ff_inlink_queued_samples(link) || ff_amix_input_want(s->mix, link)))
        ff_inlink_request_frame(link);
    else if (!s->on_event_cb){
        ff_inlink_set_status(link, AVERROR_EOF);
        for (i = 0; i < s->nb_inputs; i++) {
            if (!ff_amix_input_empty(s->mix, ctx->inputs[i])) {
                ff_filter_set_ready(ctx, 100);
                break;
            }
        }
        if (i == s->nb_inputs) {
            ff_amix_free(s->mix);
            s->mix = NULL;
        }
    }
}

static int output_frame(AVFilterContext *ctx)
{
    ABufSinkPriv *s = ctx->priv;
    AVFrame *frame = NULL;
    int i, ret;

    ret = ff_amix_read(s->mix, &frame);
    if (ret <= 0)
        return ret;

    if (s->next_pts == AV_NOPTS_VALUE)
        s->next_pts = 0;

    frame->pts = s->next_pts;
    frame->duration = av_rescale_q(frame->nb_samples, av_make_q(1, frame->sample_rate), AV_TIME_BASE_Q);
    s->next_pts += frame->duration;

    if (s->on_event_cb)
        s->on_event_cb(s->on_event_cb_udata, 0, (intptr_t)frame);
    av_frame_free(&frame);

    return 0;
}

static int abufsink_activate(AVFilterContext *ctx)
{
    ABufSinkPriv *s = ctx->priv;
    AVFilterLink *link;
    bool need_activate = true;
    int i, ret = 0;
    int64_t pts;

    for (i = 0; i < s->nb_inputs; i++) {
        link = ctx->inputs[i];

        if (!s->mix && s->on_event_cb) {
            s->mix = ff_amix_alloc(link->sample_rate, link->format, link->ch_layout.nb_channels);
            if (!s->mix)
                return AVERROR(ENOMEM);
            if (s->frame_size)
                ff_amix_set_frame_size(s->mix, s->frame_size);
        }

        if (ff_inlink_check_available_frame(link)) {
            ret = ff_amix_input_write(s->mix, link);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "input[%d] write to mix failed, ret:%d.\n", i, ret);
                continue;
            }
        }
    }

    output_frame(ctx);

    for (i = 0; i < s->nb_inputs; i++) {
        link = ctx->inputs[i];

        request_frame(ctx, i);

        if (ff_outlink_frame_wanted(link))
            need_activate = false;
    }

    if (s->on_event_cb && need_activate) //in case that all links are samples enough, no need to request frame
        ff_filter_set_ready(ctx, 100);

    return 0;
}

static int abufsink_set_parameter(AVFilterContext *ctx, const char *args)
{
    ABufSinkPriv *s = ctx->priv;
    char *key = NULL, *value = NULL;
    const char *p = args;
    int ret = 0;

    av_log(ctx, AV_LOG_INFO, "Parsing args: %s\n", args);

    while (*p) {
        ret = av_opt_get_key_value(&p, "=", ":", 0, &key, &value);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "No more key-value pairs to parse.\n");
            break;
        }
        if (*p)
            p++;
        av_log(ctx, AV_LOG_INFO, "Parsed Key: %s, Value: %s\n", key, value);
        if (!strcmp(key, "frame_size")) {
            s->frame_size = strtol(value, NULL, 0);
        } else
            av_log(ctx, AV_LOG_ERROR, "Unknown parameter: %s\n", key);

        av_freep(&key);
        av_freep(&value);
    }
    return ret;
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

        sink->frame_size = 0;
        av_abufsink_set_event_cb(ctx, NULL, NULL);
        return 0;
    } else if (!strcmp(cmd, "set_parameter")) {
        if (!args)
            return AVERROR(EINVAL);

        return abufsink_set_parameter(ctx, args);
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

    s->next_pts = AV_NOPTS_VALUE;

    return 0;
}

const AVFilter ff_asink_abufsink = {
    .name            = "abufsink",
    .description     = NULL_IF_CONFIG_SMALL("audio buffer sink(only pcm)"),
    .priv_size       = sizeof(ABufSinkPriv),
    .priv_class      = &abufsink_class,
    .init            = abufsink_init,
    FILTER_QUERY_FUNC2(abufsink_query_formats),
    .activate        = abufsink_activate,
    .process_command = abufsink_process_command,
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS,
};