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
 * audio nuttx sink
 */

#include <poll.h>

#include <libavutil/opt.h>
#include <libavutil/eval.h>
#include <libavutil/samplefmt.h>
#include <libavdevice/avdevice.h>
#include <libavdevice/nuttx.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "filters.h"
#include "avfilter.h"
#include "internal.h"

typedef struct ANxSinkPriv {
    const AVClass *class;

    NuttxPriv priv;

    char *format;
    char *devname;

    int sample_fmt;
    uint32_t sample_rate;
    AVChannelLayout ch_layout;

    int periods;
    int period_time;

    bool started;
} ANxSinkPriv;

static int anxsink_start(AVFilterContext *ctx)
{
    ANxSinkPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;
    int ret;

    if (sink->started)
        return 0;

    ret = ff_nuttx_open(priv);
    if (ret < 0)
        return ret;

    sink->started = true;
    return 0;
}

static void anxsink_stop(AVFilterContext *ctx)
{
    ANxSinkPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;;

    if (!sink->started)
        return;

    ff_nuttx_close(priv);
    priv->timestamp = 0;

    sink->started = false;
}

static int anxsink_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    ANxSinkPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    return ff_nuttx_init(priv, sink->devname, true);
}

static void anxsink_uninit(AVFilterContext *ctx)
{
    ANxSinkPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    anxsink_stop(ctx);

    if (priv->lastpkt)
        av_packet_free(&priv->lastpkt);

    ff_nuttx_deinit(priv);
}

static int devsink_write_lastpacket(AVFilterContext *ctx)
{
    ANxSinkPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;
    int ret;

    if (!priv || !priv->lastpkt)
        return 0;

    ret = ff_nuttx_write_data(priv, priv->lastpkt->data, priv->lastpkt->size);
    if (ret < 0)
        return ret;

    priv->timestamp += ret / priv->sample_bytes;

    priv->lastpkt->data += ret;
    priv->lastpkt->size -= ret;
    if (priv->lastpkt->size > 0)
        return AVERROR(EAGAIN);

    av_packet_free(&priv->lastpkt);
    return 0;
}

static int anxsink_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    ANxSinkPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;
    AVPacket *pkt;
    int ret;

    if (!frame || !frame->buf[0])
        return 0;

    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);

    pkt->buf = frame->buf[0];
    pkt->data = pkt->buf->data;
    pkt->size = frame->nb_samples * priv->sample_bytes;
    frame->buf[0] = NULL;
    frame->data[0] = NULL;
    frame->linesize[0] = 0;

    ret = ff_nuttx_write_data(priv, pkt->data, pkt->size);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to write packet! ret:%d\n", ret);
        goto out;
    }

    priv->timestamp += ret / priv->sample_bytes;

    if (ret != pkt->size) {
        priv->lastpkt = av_packet_clone(pkt);
        priv->lastpkt->data += ret;
        priv->lastpkt->size -= ret;
        ret = AVERROR(EAGAIN);
        goto out;
    }

out:
    av_packet_free(&pkt);
    return ret;
}

static int anxsink_activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFrame *frame;
    int64_t pts;
    int ret;

    ret = devsink_write_lastpacket(ctx);
    if (ret < 0)
        return ret;

    if (ff_inlink_check_available_frame(inlink)) {
        ret = anxsink_start(ctx);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
            return ret;
        }

        ret = ff_inlink_consume_frame(inlink, &frame);
        if (ret < 0)
            return ret;
        else if (ret > 0) {
            ret = anxsink_send_frame(ctx, frame);
            av_frame_free(&frame);
            if (ret >= 0)
                ff_filter_set_ready(ctx, 100);
            return ret;
        }
    }

    ff_inlink_acknowledge_status(inlink, &ret, &pts);
    if (ret >= 0)
        ff_inlink_request_frame(inlink);
    else if (ret == AVERROR_EOF)
        anxsink_stop(ctx);

    return ret;
}

static int anxsink_query_formats(AVFilterContext *ctx)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterLink *link = ctx->inputs[0];
    AVFilterFormats *formats = NULL;
    ANxSinkPriv *sink = ctx->priv;
    AVOptionRanges *ranges = NULL;
    AVChannelLayout layout;
    int ret, i;

    if (sink->sample_fmt != AV_SAMPLE_FMT_NONE) {
        ret = ff_add_format(&formats, sink->sample_fmt);
        if (ret < 0)
            goto out;
    } else {
        ret = ff_nuttx_capbility_query_ranges(&ranges, sink->devname, "sample_fmts",
                                              AV_OPT_MULTI_COMPONENT_RANGE, false);
        if (ret >= 0) {
            for (i = 0; i < ranges->nb_ranges; i++) {
                ret = ff_add_format(&formats, ranges->range[i]->value_min);
                if (ret < 0)
                    goto out;
            }
            av_opt_freep_ranges(&ranges);
        } else {
            formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
        }
    }

    ret = ff_set_common_formats(ctx, formats);
    if (ret < 0)
        goto out;

    formats = NULL;
    if (sink->sample_rate) {
        ret = ff_add_format(&formats, sink->sample_rate);
        if (ret < 0)
            goto out;
    } else {
        ret = ff_nuttx_capbility_query_ranges(&ranges, sink->devname, "sample_rates",
                                              AV_OPT_MULTI_COMPONENT_RANGE, false);
        if (ret >= 0) {
            for (i = 0; i < ranges->nb_ranges; i++) {
                ret = ff_add_format(&formats, ranges->range[i]->value_min);
                if (ret < 0)
                    goto out;
            }
            av_opt_freep_ranges(&ranges);
        }
    }

    ret = ff_set_common_samplerates(ctx, formats);
    if (ret < 0)
        goto out;

    if (sink->ch_layout.nb_channels) {
        ret = ff_add_channel_layout(&layouts, &sink->ch_layout);
        if (ret < 0)
            goto out;
    } else {
        ret = ff_nuttx_capbility_query_ranges(&ranges, sink->devname, "channels",
                                              AV_OPT_MULTI_COMPONENT_RANGE, false);
        if (ret >= 0) {
            int n;

            for (n = 0; n < ranges->nb_ranges; n++) {
                if (ranges->range[n]->is_range) {
                    for (i = ranges->range[0]->value_min; i <= ranges->range[0]->value_max; i++) {
                        av_channel_layout_default(&layout, i);
                        ret = ff_add_channel_layout(&layouts, &layout);
                        if (ret < 0)
                            goto out;
                    }
                } else {
                    i = ranges->range[n]->value_min;
                    av_channel_layout_default(&layout, i);
                    ret = ff_add_channel_layout(&layouts, &layout);
                    if (ret < 0)
                        goto out;
                }
            }

            av_opt_freep_ranges(&ranges);
        }
    }

    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        goto out;

    formats = NULL;
    ret = ff_nuttx_capbility_query_ranges(&ranges, sink->devname, "codecs",
                                          AV_OPT_MULTI_COMPONENT_RANGE, false);
    if (ret >= 0) {
        for (i = 0; i < ranges->nb_ranges; i++) {
            ret = ff_add_format(&formats, ranges->range[i]->value_min);
            if (ret < 0)
                goto out;
        }

        av_opt_freep_ranges(&ranges);
    } else {
        formats = ff_all_raw_codecs(ctx->inputs[0]->type);
    }

    ret = ff_set_common_codecs(ctx, formats);
    if (ret < 0)
        goto out;

    ret = 0;

out:
    av_opt_freep_ranges(&ranges);
    return ret;
}

static int anxsink_control_message(AVFilterContext *ctx, int type,
                                   void *data, size_t data_size)
{
    if (type == AV_DEV_TO_APP_BUFFER_WRITABLE)
        ff_filter_set_ready(ctx, 100);
    else if (type == AV_DEV_TO_APP_BUFFER_DRAINED)
        avfilter_forward_command(ctx, 0, NULL,
                                 "completed", NULL, NULL,
                                 0, AVFILTER_CMD_FLAG_REVERSE);
    else if (type == AV_DEV_TO_APP_STATE_CHANGED) {
      avfilter_graph_reconfig(ctx->graph, ctx);
      ff_filter_set_ready(ctx, 100);
    }

    return 0;
}

static int anxsink_process_command(AVFilterContext *ctx,
                                   const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    ANxSinkPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    if (!priv)
        return 0;

    if (!strcmp(cmd, "start")) {
        priv->stopped = false;
        anxsink_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
        return 0;
    } else if (!strcmp(cmd, "stop")) {
        priv->stopped = true;
        anxsink_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
        return 0;
    } else if (!strcmp(cmd, "play")) {
        return ff_nuttx_resume(priv);
    }  else if (!strcmp(cmd, "pause")) {
        return ff_nuttx_pause(priv);
    } else if (!strcmp(cmd, "get_position")) {
        long ret = ff_nuttx_get_position(priv);
        if (ret < 0)
          return ret;
        snprintf(res, res_len, "%ld", ret);
        return 0;
    } else if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll = (struct pollfd *)res;

        if (!res || res_len < sizeof(struct pollfd))
            return AVERROR(EINVAL);

        poll[0].fd = priv->mq;
        poll[0].events = POLLIN;

        return 1;
    } else if (!strcmp(cmd, "poll_available")) {
        enum AVDevToAppMessageType t;
        int ret;

        ret = ff_nuttx_poll_available(priv);
        t = ret == AVERROR_EXIT ? AV_DEV_TO_APP_BUFFER_DRAINED: AV_DEV_TO_APP_BUFFER_WRITABLE;
        anxsink_control_message(ctx, t, NULL, 0);
        return ret;
    } else if (!strcmp(cmd, "set_parameter")) {
        return ff_nuttx_set_parameter(priv, args);
    } else if (!strcmp(cmd, "flush")) {
        if (priv->lastpkt)
            av_packet_free(&priv->lastpkt);
        return ff_nuttx_flush(priv);
    } else if (!strcmp(cmd, "dump")) {
        snprintf(res, res_len, "%d|%d|%d|%d|%zu|%d",
                     priv->running, priv->draining,
                     priv->period_bytes, priv->periods,
                     dq_count(&priv->bufferq), priv->mq);
        return 0;
    } else if (!strcmp(cmd, "get_timestamp")) {
        int64_t *ts  = ((int64_t **)res)[0];
        int64_t *lat = ((int64_t **)res)[1];
        long latency;

        latency = ff_nuttx_get_latency(priv);
        latency = FFMAX(latency, 0);

        if (priv->lastpkt)
            latency += priv->lastpkt->size / priv->sample_bytes;

        *ts = priv->timestamp - latency;
        *lat = latency;

        *ts  = av_rescale_q(*ts,  ctx->inputs[0]->time_base, AV_TIME_BASE_Q);
        *lat = av_rescale_q(*lat, ctx->inputs[0]->time_base, AV_TIME_BASE_Q);

        return 0;
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int anxsink_forward_command(AVFilterContext *ctx, int pad_idx, const char* target, const char *cmd,
                                   const char *arg, char *res, int res_len, int flags)
{
    return anxsink_process_command(ctx, cmd, arg, res, res_len, flags);
}

static int anxsink_config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    ANxSinkPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    priv->periods = sink->periods;
    priv->period_time = sink->period_time;

    priv->nonblock = true;
    priv->codec = av_get_pcm_codec(link->format, -1);
    priv->sample_rate = link->sample_rate;
    priv->format = link->format;
    priv->ch_layout.nb_channels = link->ch_layout.nb_channels;

    return 0;
}

#define OFFSET(x) offsetof(ANxSinkPriv, x)
#define FLAGS  AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define FLAGSR FLAGS|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption anxsink_options[] = {
    { "format",      "", OFFSET(format),      AV_OPT_TYPE_STRING,     .flags = FLAGS },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = FLAGS },
    { "sample_fmt",  "", OFFSET(sample_fmt),  AV_OPT_TYPE_SAMPLE_FMT, {.i64=AV_SAMPLE_FMT_NONE}, -1, INT_MAX, FLAGSR },
    { "sample_rate", "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                 0, INT_MAX, FLAGSR },
    { "ch_layout",   "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},              0, 0,       FLAGSR },
    { "periods",     "", OFFSET(periods),     AV_OPT_TYPE_INT,        {.i64 = 4},                 0, INT_MAX, FLAGSR },
    { "period_time", "", OFFSET(period_time), AV_OPT_TYPE_INT,        {.i64 = 20},                0, INT_MAX, FLAGSR },
    { NULL },
};

static const AVClass anxsink_class = {
    .class_name          = "anxsink_class",
    .item_name           = av_default_item_name,
    .option              = anxsink_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
};

static const AVFilterPad anxsink_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
        .config_props  = anxsink_config_props,
    },
};

const AVFilter ff_asink_anxsink = {
    .name            = "anxsink",
    .description     = NULL_IF_CONFIG_SMALL("Audio Nuttx sink(only pcm)"),
    .priv_class      = &anxsink_class,
    .priv_size       = sizeof(ANxSinkPriv),
    .init_dict       = anxsink_init_dict,
    .uninit          = anxsink_uninit,
    .activate        = anxsink_activate,
    FILTER_INPUTS(anxsink_inputs),
    FILTER_QUERY_FUNC(anxsink_query_formats),
    .process_command = anxsink_process_command,
    .forward_command = anxsink_forward_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
