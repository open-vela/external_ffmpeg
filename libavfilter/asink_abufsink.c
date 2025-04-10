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

#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "internal.h"
#include "formats.h"

typedef struct ABufSinkPriv {
    const AVClass *class;

    int (*on_event_cb)(void *udata, int evt, int64_t args);
    void *on_event_cb_udata;
} ABufSinkPriv;

static void av_abufsink_set_event_cb(AVFilterContext *ctx,
    int (*on_event_cb)(void *udata, int evt, int64_t args), void *udata)
{
    ABufSinkPriv *s = ctx->priv;

    s->on_event_cb = on_event_cb;
    s->on_event_cb_udata = udata;

    ff_filter_set_ready(ctx, 100);
}

static int abufsink_activate(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->inputs[0];
    ABufSinkPriv *s = ctx->priv;
    AVFrame *frame = NULL;
    int ret = 0;

    if (ff_inlink_queued_frames(link)) {
        ret = ff_inlink_consume_frame(link, &frame);
        if (ret < 0)
            goto out;

        if (s->on_event_cb)
            s->on_event_cb(s->on_event_cb_udata, 0, (intptr_t)frame);
    }

    if (s->on_event_cb)
        ff_inlink_request_frame(link);
    else {
        if (!ff_inlink_queued_frames(link)) {
            ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
        } else {
            ff_filter_set_ready(ctx, 100);
        }
    }

out:
    av_frame_free(&frame);

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

    formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
    ret = ff_set_common_formats2(ctx, cfg_in, cfg_out, formats);
    if (ret < 0)
        goto out;

    formats = ff_all_samplerates();
    ret = ff_set_common_samplerates2(ctx, cfg_in, cfg_out, formats);
    if (ret < 0)
        goto out;

    layouts = ff_all_channel_counts();
    ret = ff_set_common_channel_layouts2(ctx, cfg_in, cfg_out, layouts);
    if (ret < 0)
        goto out;

out:
    return ret;
}

const AVFilterPad abufsink_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = NULL,
    },
};

const AVFilter ff_asink_abufsink = {
    .name            = "abufsink",
    .description     = NULL_IF_CONFIG_SMALL("audio buffer sink(only pcm)"),
    .priv_size       = sizeof(ABufSinkPriv),
    .priv_class      = NULL,
    .init            = NULL,
    .uninit          = NULL,
    FILTER_INPUTS(abufsink_inputs),
    FILTER_QUERY_FUNC2(abufsink_query_formats),
    .activate        = abufsink_activate,
    .process_command = abufsink_process_command,
};