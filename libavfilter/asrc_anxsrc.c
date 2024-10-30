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
 * audio nuttx source
 */

#include <poll.h>

#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavdevice/nuttx.h>
#include <libavformat/internal.h>
#include <libavformat/demux.h>
#include <libavcodec/avcodec.h>

#include "avfilter.h"
#include "filters.h"
#include "internal.h"
#include "packet_wrapper.h"

typedef struct ANxSrcPriv {
    const AVClass *class;

    NuttxPriv priv;

    char *format;
    char *devname;

    int sample_fmt;
    uint32_t sample_rate;
    AVChannelLayout ch_layout;

    int periods;
    int period_time;
} ANxSrcPriv;

static int anxsrc_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    ANxSrcPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    memset(priv, 0, sizeof(NuttxPriv));

    return ff_nuttx_init(priv, sink->devname, false);
}

static void anxsrc_uninit(AVFilterContext *ctx)
{
    ANxSrcPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    ff_nuttx_deinit(priv);
}

static void anxsrc_close(AVFilterContext *ctx)
{
    ANxSrcPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    if (!priv->running)
        return;

    ff_nuttx_close(priv);
}

static int anxsrc_open(AVFilterContext *ctx)
{
    ANxSrcPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;
    int ret;

    if (priv->running)
        return 0;

    ret = ff_nuttx_open(priv);
    if (ret < 0)
        return ret;

    return avfilter_graph_reconfig(ctx->graph, ctx);
}

static inline void anxsrc_force_request(AVFilterContext *ctx)
{
    ctx->outputs[0]->frame_wanted_out = 1;
    ff_filter_set_ready(ctx, 300);
}

static int anxsrc_control_message(AVFilterContext *ctx, int type,
                                  void *data, size_t data_size)
{
    ANxSrcPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    if (type == AV_DEV_TO_APP_STATE_CHANGED) {
        avfilter_graph_reconfig(ctx->graph, ctx);
    }

    if (type == AV_DEV_TO_APP_STATE_CHANGED ||
        type == AV_DEV_TO_APP_BUFFER_READABLE) {
        if (priv->running)
            ff_filter_set_ready(ctx, 300);
        else
            anxsrc_force_request(ctx);
    }

    return 0;
}

static int anxsrc_wrap_frame(AVFilterContext *ctx, AVFrame **frame)
{
    AVFilterLink *link = ctx->outputs[0];
    ANxSrcPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;
    AVFrame *out = NULL;
    uint32_t samples;
    AVPacket *pkt;
    int ret;

    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);

    ret = av_new_packet(pkt, priv->period_bytes);
    if (ret < 0)
        return ret;

    ret = ff_nuttx_read_data(priv, pkt->data, priv->period_bytes, &samples);
    if (ret < 0)
        goto error;

    pkt->size = ret;
    pkt->pts = priv->timestamp;
    priv->timestamp += samples > 0 ? samples : ret / priv->sample_bytes;

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    out->format = link->format;
    out->sample_rate = link->sample_rate;
    av_channel_layout_copy(&out->ch_layout, &link->ch_layout);
#if FF_API_OLD_CHANNEL_LAYOUT
    out->channel_layout = link->ch_layout.u.mask;
    out->channels = link->ch_layout.nb_channels;
#endif
    out->nb_samples = pkt->size / priv->sample_bytes;
    out->pkt_size = pkt->size;

    out->buf[0] = pkt->buf;
    out->data[0] = out->buf[0]->data;
    out->linesize[0] = pkt->size;
    out->extended_data = out->data;
    out->pts = pkt->pts;

    pkt->buf = NULL;
    pkt->data = NULL;
    pkt->size = 0;
    av_packet_free(&pkt);

    *frame = out;
    return 0;

error:
    av_packet_free(&pkt);
    av_frame_free(&out);
    return ret;
}

static int anxsrc_activate(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->outputs[0];
    AVFrame *frame;
    int ret;

    ret = ff_outlink_get_status(link);
    if (ret < 0) {
        if (ret == AVERROR_EOF)
            anxsrc_close(ctx);
        return ret;
    }

    if (!ff_outlink_frame_wanted(link))
        return FFERROR_NOT_READY;

    ret = anxsrc_open(ctx);
    if (ret < 0)
        goto out;

    ret = anxsrc_wrap_frame(ctx, &frame);
    if (ret < 0)
        goto out;

    return ff_filter_frame(link, frame);

out:
    if (ret == AVERROR_EOF) {
        anxsrc_close(ctx);
        ff_avfilter_link_set_in_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
    }

    return ret;
}

static int anxsrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                  char *res, int res_len, int flags)
{
    ANxSrcPriv *sink = ctx->priv;
    NuttxPriv *priv = &sink->priv;

    if (!strcmp(cmd, "start")) {
        priv->stopped = false;
        anxsrc_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
        return 0;
    } else if (!strcmp(cmd, "stop")) {
        priv->stopped = true;
        anxsrc_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
        return 0;
    } else if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll = (struct pollfd *)res;

        if (!res || res_len < sizeof(struct pollfd))
          return AVERROR(EINVAL);

        poll[0].fd = priv->mq;
        poll[0].events = POLLIN;

        return 1;
    } else if (!strcmp(cmd, "poll_available")) {
        int ret;

        ret = ff_nuttx_poll_available(priv);
        anxsrc_control_message(ctx, AV_DEV_TO_APP_BUFFER_READABLE, NULL, 0);
        return ret;
    } else if (!strcmp(cmd, "set_parameter")) {
        return ff_nuttx_set_parameter(priv, args);
    } else if (!strcmp(cmd, "mute")) {
        priv->mute = true;
        return 0;
    } else if (!strcmp(cmd, "unmute")) {
        priv->mute = false;
        return 0;
    } else if (!strcmp(cmd, "dump")) {
        snprintf(res, res_len, "%d|%d|%d|%d|%zu|%d", priv->running,
                 priv->draining, priv->period_bytes, priv->periods,
                 dq_count(&priv->bufferq), priv->mq);
        return 0;
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int anxsrc_query_formats(AVFilterContext *ctx)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    ANxSrcPriv *sink = ctx->priv;
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
            for (i = 0; i < ranges->nb_ranges; i++) {
                if (ranges->range[i]->is_range) {
                    for (i = ranges->range[0]->value_min; i <= ranges->range[0]->value_max; i++) {
                        av_channel_layout_default(&layout, i);
                        ret = ff_add_channel_layout(&layouts, &layout);
                        if (ret < 0)
                            goto out;
                    }
                } else {
                    i = ranges->range[i]->value_min;
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

static int anxsrc_config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    ANxSrcPriv *sink = ctx->priv;
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

#define OFFSET(x) offsetof(ANxSrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption anxsrc_options[] = {
    { "format",      "", OFFSET(format),      AV_OPT_TYPE_STRING,     .flags = A },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = A },
    { "sample_fmt",  "", OFFSET(sample_fmt),  AV_OPT_TYPE_SAMPLE_FMT, {.i64=AV_SAMPLE_FMT_NONE}, -1, INT_MAX, R },
    { "sample_rate", "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                 0, INT_MAX, R },
    { "ch_layout",   "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},              0, 0,       R },
    { "periods",     "", OFFSET(periods),     AV_OPT_TYPE_INT,        {.i64 = 4},                 0, INT_MAX, R },
    { "period_time", "", OFFSET(period_time), AV_OPT_TYPE_INT,        {.i64 = 20},                0, INT_MAX, R },
    { NULL },
};

static const AVClass anxsrc_class = {
    .class_name          = "anxsrc_class",
    .item_name           = av_default_item_name,
    .option              = anxsrc_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
};

static const AVFilterPad anxsrc_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = anxsrc_config_props,
    },
};

const AVFilter ff_asrc_anxsrc = {
    .name            = "anxsrc",
    .description     = NULL_IF_CONFIG_SMALL("audio nuttx source(only pcm)"),
    .priv_size       = sizeof(ANxSrcPriv),
    .priv_class      = &anxsrc_class,
    .init_dict       = anxsrc_init_dict,
    .uninit          = anxsrc_uninit,
    FILTER_OUTPUTS(anxsrc_outputs),
    FILTER_QUERY_FUNC(anxsrc_query_formats),
    .activate        = anxsrc_activate,
    .process_command = anxsrc_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
