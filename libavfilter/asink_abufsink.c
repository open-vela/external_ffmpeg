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

#define SINK_STATE_IDLE    0    /**< sink is idle (not linked) */
#define SINK_STATE_RUNNING 1    /**< sink is running (linked and active) */
#define SINK_STATE_PAUSED  2    /**< sink is paused (linked but inactive) */

typedef struct ABufSinkPriv {
    const AVClass *class;

    int nb_inputs;                  /**< number of inputs */

    int64_t next_pts;               /**< calculated pts for next output frame */

    int sample_rate;                /**< sample rate */
    AVChannelLayout ch_layout;      /**< channel layout */
    enum AVSampleFormat sample_fmt; /**< sample format */

    int state;                      /**< sink state: IDLE/RUNNING/PAUSED */

    AMixContext *mix;               /**< mix module context */
    int frame_size;                 /**< frame size */

    int (*on_event_cb)(void *udata, int evt, int64_t args);
    void *on_event_cb_udata;
} ABufSinkPriv;

#define OFFSET(x) offsetof(ABufSinkPriv, x)
#define A AV_OPT_FLAG_AUDIO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM
static const AVOption abufsink_options[] = {
    { "inputs", "Number of inputs.",
            OFFSET(nb_inputs), AV_OPT_TYPE_INT, {.i64 = 1}, 1, INT16_MAX, A|F },
    { "state", "Sink state (0=IDLE, 1=RUNNING, 2=PAUSED).",
            OFFSET(state), AV_OPT_TYPE_INT, {.i64 = SINK_STATE_IDLE}, SINK_STATE_IDLE, SINK_STATE_PAUSED, A|F },
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

static void request_frame(AVFilterContext *ctx)
{
    ABufSinkPriv *s = ctx->priv;
    bool activate = true;
    AVFilterLink *link;
    int ret, pad, count = 0;
    int64_t pts;

    for (pad = 0; pad < ctx->nb_inputs; pad++) {
        link = ctx->inputs[pad];

        ff_inlink_acknowledge_status(link, &ret, &pts);
        if (ret < 0){
            /* In case that all inputs are in EOF, but there is data remains in link. */
            if (ff_amix_input_empty(s->mix, link))
                count++;
            continue;
        }

        if (!s->mix || ff_amix_input_want(s->mix, link)) {
            ff_inlink_request_frame(link);
            activate = false;
        }
    }

    if (activate && count < ctx->nb_inputs)
        ff_filter_set_ready(ctx, 100);
}

static int output_frame(AVFilterContext *ctx)
{
    ABufSinkPriv *s = ctx->priv;
    AVFrame *frame = NULL;
    int ret;

    if (!s->mix)
        return AVERROR(EINVAL);

    ret = ff_amix_read(s->mix, &frame);
    if (ret <= 0)
        return ret;

    if (s->on_event_cb && s->state == SINK_STATE_RUNNING) {
        if (s->next_pts == AV_NOPTS_VALUE)
            s->next_pts = 0;

        frame->pts = s->next_pts;
        frame->duration = av_rescale_q(frame->nb_samples, av_make_q(1, frame->sample_rate), AV_TIME_BASE_Q);
        s->next_pts += frame->duration;

        s->on_event_cb(s->on_event_cb_udata, 0, (intptr_t)frame);
    }

    av_frame_free(&frame);

    return 0;
}

static int abufsink_activate(AVFilterContext *ctx)
{
    ABufSinkPriv *s = ctx->priv;
    AVFilterLink *link;
    int i, ret = 0;
    int64_t pts;

    for (i = 0; i < s->nb_inputs; i++) {
        link = ctx->inputs[i];

        if (ff_inlink_check_available_frame(link)) {
            if (!s->mix && s->on_event_cb) {
                s->mix = ff_amix_alloc(s->sample_rate, s->sample_fmt, s->ch_layout.nb_channels);
                if (!s->mix)
                    return AVERROR(ENOMEM);
                if (s->frame_size)
                    ff_amix_set_frame_size(s->mix, s->frame_size);
            }

            ret = ff_amix_input_write(s->mix, link);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "input[%d] write to mix failed, ret:%d.\n", i, ret);
                continue;
            }
        }
    }

    output_frame(ctx);

    if (s->on_event_cb)
        request_frame(ctx);
    else {
        for (i = 0; i < s->nb_inputs; i++) {
            link = ctx->inputs[i];
            ff_inlink_set_status(link, AVERROR_EOF);
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
            av_log(ctx, AV_LOG_ERROR, "Unable to parse '%s': %s\n", p, av_err2str(ret));
            break;
        }
        if (*p)
            p++;
        av_log(ctx, AV_LOG_INFO, "Parsed Key: %s, Value: %s\n", key, value);
        if (!strcmp(key, "frame_size"))
            s->frame_size = strtol(value, NULL, 0);
        else
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
        int format, sample_rate, channels;
        void *udata;

        if (!args)
            return AVERROR(EINVAL);

        if (sscanf(args, "%p %p fmt=%d:rate=%d:ch=%d", &on_event_cb, &udata, &format, &sample_rate, &channels) != 5)
            return AVERROR(EINVAL);

        sink->sample_fmt = format;
        sink->sample_rate = sample_rate;
        av_channel_layout_default(&sink->ch_layout, channels);

        if (!sink->on_event_cb)
            av_abufsink_set_event_cb(ctx, on_event_cb, udata);
        sink->state = SINK_STATE_RUNNING;
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        if (sink->on_event_cb)
            sink->on_event_cb(sink->on_event_cb_udata, -1, 0);

        sink->frame_size = 0;
        sink->next_pts = AV_NOPTS_VALUE;
        sink->sample_fmt = AV_SAMPLE_FMT_NONE;
        sink->sample_rate = 0;
        av_channel_layout_uninit(&sink->ch_layout);
        av_abufsink_set_event_cb(ctx, NULL, NULL);
        sink->state = SINK_STATE_IDLE;
        return 0;
    } else if (!strcmp(cmd, "set_parameter")) {
        if (!args)
            return AVERROR(EINVAL);

        return abufsink_set_parameter(ctx, args);
    } else if (!av_strcasecmp(cmd, "pause")) {
        sink->state = SINK_STATE_PAUSED;
        return 0;
    } else if (!av_strcasecmp(cmd, "resume")) {
        sink->state = SINK_STATE_RUNNING;
        ff_filter_set_ready(ctx, 100);
        return 0;
    }

    return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
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
    s->state = SINK_STATE_IDLE;

    return 0;
}

static int abufsink_query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in,
                               AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    ABufSinkPriv *sink = ctx->priv;
    int ret = AVERROR(EINVAL), i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        AVChannelLayout list64[] = { { 0 }, { 0 } };
        int list[] = { 0, -1 };

        list[0] = sink->sample_fmt;
        formats = ff_make_format_list(list);
        if (!formats)
            goto out;

        ff_formats_unref(&cfg_in[i]->formats);
        ret = ff_formats_ref(formats, &cfg_in[i]->formats);
        if (ret < 0)
            goto out;

        formats = NULL;
        list[0] = sink->sample_rate;
        formats = ff_make_format_list(list);
        if (!formats)
            goto out;

        ff_formats_unref(&cfg_in[i]->samplerates);
        ret = ff_formats_ref(formats, &cfg_in[i]->samplerates);
        if (ret < 0)
            goto out;

        list64[0] = sink->ch_layout;
        layouts = ff_make_channel_layout_list(list64);
        if (!layouts)
            goto out;

        ff_channel_layouts_unref(&cfg_in[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_in[i]->channel_layouts);
    }

out:
    return ret;
}

const AVFilter ff_asink_abufsink = {
    .name            = "abufsink",
    .description     = NULL_IF_CONFIG_SMALL("audio buffer sink(only pcm)"),
    .priv_size       = sizeof(ABufSinkPriv),
    .priv_class      = &abufsink_class,
    .init            = abufsink_init,
    .activate        = abufsink_activate,
    .process_command = abufsink_process_command,
    FILTER_QUERY_FUNC2(abufsink_query_formats),
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS,
};
