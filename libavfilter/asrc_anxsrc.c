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

#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavdevice/nuttx.h>
#include <libavformat/internal.h>
#include <libavformat/demux.h>
#include <libavcodec/avcodec.h>
#include "libavutil/mem.h"

#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "internal.h"
#include "formats.h"
#include "aresample.h"
#include "mapping.h"

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

    char *map_str;
    int *map;
    int nb_outputs;

    bool running;

    AResampleContext *resample; /**< resampler context for audio output */
} ANxSrcPriv;

static int anxsrc_config_props(AVFilterLink *link)
{
    av_log(link->src, AV_LOG_INFO, "link sample format: %s, sample_rate %d, channels %d.\n",
           av_get_sample_fmt_name(link->format), link->sample_rate,
           link->ch_layout.nb_channels);
    return 0;
}

static int anxsrc_get_device_support_format(AVFilterContext *ctx, const char *devname,
                                            const char *key, int value, int *out_value)
{
    AVOptionRanges* ranges = NULL;
    AVOptionRange* range = NULL;
    int ret, range_idx;

    ret = ff_nuttx_capbility_query_ranges(&ranges, devname, key, 0, false);
    if (ret > 0) {
        for (range_idx = 0; range_idx < ranges->nb_ranges; range_idx++) {
            range = ranges->range[range_idx];
            if ((range->is_range && value >= range->value_min &&
                 value <= range->value_max) ||
                (!range->is_range && range->value_min == value)) {
                break;
            }
        }

        if (range_idx == ranges->nb_ranges) {
            av_log(ctx, AV_LOG_WARNING, "Unsupported %s: %d\n", key, value);
            *out_value = range->value_min;
        } else {
            *out_value = value;
        }
        av_opt_freep_ranges(&ranges);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Unsupported query %s: %d\n", key, value);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int anxsrc_config_output_formats(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    ANxSrcPriv *src = ctx->priv;
    NuttxPriv *priv = &src->priv;
    int ret;

    priv->periods = src->periods;
    priv->period_time = src->period_time;
    priv->nonblock = true;

    ret = anxsrc_get_device_support_format(ctx, src->devname, "sample_fmts",
                                           link->format, &priv->format);
    if (ret < 0)
        return ret;

    priv->codec = av_get_pcm_codec(priv->format, -1);

    ret = anxsrc_get_device_support_format(ctx, src->devname, "sample_rates",
                                           link->sample_rate, &priv->sample_rate);
    if (ret < 0)
        return ret;

    ret = anxsrc_get_device_support_format(ctx, src->devname, "channels",
                                           link->ch_layout.nb_channels,
                                           &priv->ch_layout.nb_channels);
    if (ret < 0)
        return ret;

    if (priv->ch_layout.nb_channels == link->ch_layout.nb_channels)
        av_channel_layout_copy(&priv->ch_layout, &link->ch_layout);
    else
        av_channel_layout_default(&priv->ch_layout, priv->ch_layout.nb_channels);

    return 0;
}

static int anxsrc_init_dict(AVFilterContext *ctx)
{
    ANxSrcPriv *src = ctx->priv;
    NuttxPriv *priv = &src->priv;
    int i, ret;

    for (i = 0; i < src->nb_outputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        pad.config_props = anxsrc_config_props;
        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    if (src->map_str) {
        ret = avfilter_parse_mapping(src->map_str, &src->map, src->nb_outputs);
        if (ret < 0)
            return ret;
    }

    src->resample = av_mallocz(sizeof(AResampleContext) * src->nb_outputs);
    if (!src->resample)
        return AVERROR(ENOMEM);

    for (i = 0; i < src->nb_outputs; i++)
        ff_resample_init(&src->resample[i]);

    return ff_nuttx_init(priv, src->devname, false);
}

static void anxsrc_uninit(AVFilterContext *ctx)
{
    ANxSrcPriv *src = ctx->priv;
    NuttxPriv *priv = &src->priv;
    int i;
    for (i = 0; i < src->nb_outputs; i++)
        ff_resample_uninit(&src->resample[i]);

    av_freep(&src->resample);
    ff_nuttx_deinit(priv);
    av_freep(&src->map);
}

static int anxsrc_open(AVFilterContext *ctx)
{
    ANxSrcPriv *src = ctx->priv;
    NuttxPriv *priv = &src->priv;
    int ret;
    int i;

    if (priv->running)
        return 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (src->map && src->map[i] < 0)
            continue;

        anxsrc_config_output_formats(ctx->outputs[i]);
        break;
    }

    ret = ff_nuttx_open(priv);
    if (ret < 0)
        return ret;

    src->running = true;
    return 0;
}

static inline void anxsrc_force_request(AVFilterContext *ctx)
{
    ANxSrcPriv *s = ctx->priv;
    FilterLinkInternal *li;
    int i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (s->map && s->map[i] < 0)
            continue;

        li = ff_link_internal(ctx->outputs[i]);
        li->frame_wanted_out = 1;
        ff_filter_set_ready(ctx, 300);
    }
}

static int anxsrc_control_message(AVFilterContext *ctx, int type,
                                  void *data, size_t data_size)
{
    if (type == AV_DEV_TO_APP_STATE_CHANGED) {
        //avfilter_graph_reconfig(ctx->graph, ctx);
    }

    if (type == AV_DEV_TO_APP_STATE_CHANGED ||
        type == AV_DEV_TO_APP_BUFFER_READABLE) {
        anxsrc_force_request(ctx);
    }

    return 0;
}

static int anxsrc_read_packet(AVFilterContext *ctx, AVPacket **p)
{
    ANxSrcPriv *s = ctx->priv;
    NuttxPriv *priv = &s->priv;
    uint32_t samples;
    AVPacket *pkt;
    int ret;

    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);

    ret = av_new_packet(pkt, priv->period_bytes);
    if (ret < 0) {
        av_packet_free(&pkt);
        return ret;
    }

    ret = ff_nuttx_read_data(priv, pkt->data, priv->period_bytes, &samples);
    if (ret < 0)
        goto error;

    pkt->size = ret;
    pkt->pts = priv->timestamp;
    priv->timestamp += samples > 0 ? samples : ret / priv->sample_bytes;
    *p = pkt;

    return 0;

error:
    av_packet_free(&pkt);
    return ret;
}

static int anxsrc_wrap_frame(AVFilterContext *ctx, int pad, AVPacket **pkt, AVFrame **frame)
{
    AVFilterLink *link = ctx->outputs[pad];
    ANxSrcPriv *s = ctx->priv;
    NuttxPriv *priv = &s->priv;
    AVFrame *src;
    int ret;

    src = av_frame_alloc();
    if (!src) {
        return AVERROR(ENOMEM);
    }

    src->format = priv->format;
    src->sample_rate = priv->sample_rate;
    av_channel_layout_copy(&src->ch_layout, &priv->ch_layout);
    src->nb_samples = (*pkt)->size / priv->sample_bytes;
    src->pkt_size = (*pkt)->size;

    src->buf[0] = (*pkt)->buf;
    src->data[0] = src->buf[0]->data;
    src->linesize[0] = (*pkt)->size;
    src->extended_data = src->data;
    src->pts = (*pkt)->pts;

    ret = ff_resample_frame(&s->resample[pad], link, src, frame);
    if (ret == 0)
        *frame = src;
    else
        av_frame_free(&src);

    (*pkt)->buf = NULL;
    (*pkt)->data = NULL;
    (*pkt)->size = 0;
    av_packet_free(pkt);

    return ret;
}

static int anxsrc_activate(AVFilterContext *ctx)
{
    ANxSrcPriv *s = ctx->priv;
    NuttxPriv *priv = &s->priv;
    AVPacket *pkt = NULL;
    int i, ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ff_outlink_frame_wanted(ctx->outputs[i]))
            break;
    }
    if (i == ctx->nb_outputs)
        return FFERROR_NOT_READY;

    ret = anxsrc_open(ctx);
    if (ret < 0)
        goto out;

    ret = anxsrc_read_packet(ctx, &pkt);
    if (ret < 0)
        goto out;

    for (i = 0; i < ctx->nb_outputs; i++) {
        AVFrame *frame = NULL;
        AVFilterLink *link;
        AVPacket *pkt_out;

        if (s->map && s->map[i] < 0)
            continue;

        pkt_out = av_packet_clone(pkt);
        if (!pkt_out) {
            ret = AVERROR(ENOMEM);
            goto out;
        }

        ret = anxsrc_wrap_frame(ctx, i, &pkt_out, &frame);
        if (ret < 0)
            goto out;

        link = ctx->outputs[i];
        ret = ff_filter_frame(link, frame);
        if (ret < 0)
            goto out;
    }

out:
    if (pkt != NULL) {
        av_packet_free(&pkt);
    }

    return ret;
}

static int anxsrc_send_empty_frame(AVFilterContext *ctx, AVFilterLink *link)
{
    AVFrame *frame = av_frame_alloc();
    int ret;

    if (!frame)
        return AVERROR(ENOMEM);

    frame->nb_samples = 0;
    frame->format = link->format;
    frame->sample_rate = link->sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &link->ch_layout);

    ret = ff_filter_frame(link, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    return 0;
}

static int anxsrc_get_parameter(AVFilterContext *ctx, const char *key, char *value, int value_len)
{
    ANxSrcPriv *src = ctx->priv;

    if (!strcmp(key, "get_format")) {
        AVFilterLink* link;
        int i;

        for (i = 0; i < ctx->nb_outputs; i++) {
            if (src->map && src->map[i] == 0)
                break;
        }

        if (i == ctx->nb_outputs)
            snprintf(value, value_len, "fmt=0:rate=0:ch=0");
        else
          snprintf(value, value_len, "fmt=%d:rate=%d:ch=%d", ctx->outputs[i]->format, ctx->outputs[i]->sample_rate,
                    ctx->outputs[i]->ch_layout.nb_channels);
        return 0;
    }

    av_log(ctx, AV_LOG_ERROR, "get_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

static int anxsrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                  char *res, int res_len, int flags)
{
    ANxSrcPriv *src = ctx->priv;
    NuttxPriv *priv = &src->priv;
    int ret;

    if (!strcmp(cmd, "link")) {
        anxsrc_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        AVFilterLink* link;
        int i;

        if (sscanf(args, "%p", &link) != 1)
            return AVERROR(EINVAL);

        ret = anxsrc_send_empty_frame(ctx, link);
        if (ret < 0)
            av_log(ctx, AV_LOG_ERROR, "send empty frame failed:%d\n", ret);

        if (i == ctx->nb_outputs && priv->running) {
            ff_nuttx_close(priv);
            src->running = false;
        }

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
    } else if (!av_strcasecmp(cmd, "map")) {
        int *old_map = NULL;
        int i;

        if (src->map) {
            old_map = av_calloc(src->nb_outputs, sizeof(*old_map));
            if (!old_map)
                return AVERROR(ENOMEM);

            memcpy(old_map, src->map, src->nb_outputs * sizeof(*old_map));
        }

        ret = avfilter_parse_mapping(args, &src->map, src->nb_outputs);
        if (ret < 0) {
            av_freep(&old_map);
            return ret;
        }

        for (i = 0; i < src->nb_outputs && old_map; i++) {
            if (old_map[i] == ROUTE_ON && src->map[i] == ROUTE_OFF &&
                ff_outlink_frame_wanted(ctx->outputs[i])) {
                ret = anxsrc_send_empty_frame(ctx, ctx->outputs[i]);
                if (ret < 0) {
                    av_freep(&old_map);
                    return ret;
                }
            }

            if (old_map[i] != src->map[i])
                ff_filter_set_ready(ctx, 100);
        }

        if (i == src->nb_outputs) {
            ff_nuttx_close(priv);
            src->running = false;
        }

        av_freep(&old_map);

        return ret;
    } else if (!strcmp(cmd, "force_request")){
        anxsrc_force_request(ctx);
        return 0;
    } else if (!strcmp(cmd, "get_parameter")){
        char key[32];

        if (sscanf(args, "%*p %31s", key) != 1)
            return AVERROR(EINVAL);

        return anxsrc_get_parameter(ctx, key, res, res_len);
    } else
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
}

static int anxsrc_query_formats(const AVFilterContext *ctx,
                                AVFilterFormatsConfig **cfg_in,
                                AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    ANxSrcPriv *src = ctx->priv;
    AVChannelLayout layout;
    int ret, i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        int list[] = { 0, -1 };

        if (src->sample_fmt != AV_SAMPLE_FMT_NONE) {
            list[0] = src->sample_fmt;
            formats = ff_make_format_list(list);
            if (!formats)
                goto out;
        } else
            formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);

        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(formats, &cfg_out[i]->formats);
        if (ret < 0)
            goto out;

        formats = NULL;

        if (src->sample_rate) {
            list[0] = src->sample_rate;
            formats = ff_make_format_list(list);
            if (!formats)
                goto out;
        } else
            formats = ff_all_samplerates();

        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(formats, &cfg_out[i]->samplerates);
        if (ret < 0)
            goto out;

        if (src->ch_layout.nb_channels) {
            AVChannelLayout list64[] = { { 0 }, { 0 } };

            list64[0] = src->ch_layout;
            layouts = ff_make_channel_layout_list(list64);
            if (!layouts)
                goto out;
        } else
            layouts = ff_all_channel_counts();

        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_out[i]->channel_layouts);
    }

out:
    return ret;
}

#define OFFSET(x) offsetof(ANxSrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption anxsrc_options[] = {
    { "format",      "", OFFSET(format),      AV_OPT_TYPE_STRING,     .flags = A },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = A },
    { "sample_fmt",  "", OFFSET(sample_fmt),  AV_OPT_TYPE_SAMPLE_FMT, {.i64=AV_SAMPLE_FMT_NONE},  -1, INT_MAX, R },
    { "sample_rate", "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                  0, INT_MAX, R },
    { "ch_layout",   "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},               0, 0,       R },
    { "periods",     "", OFFSET(periods),     AV_OPT_TYPE_INT,        {.i64 = 4},                  0, INT_MAX, R },
    { "period_time", "", OFFSET(period_time), AV_OPT_TYPE_INT,        {.i64 = 20},                 0, INT_MAX, R },
    { "outputs",     "", OFFSET(nb_outputs),  AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "map",         "", OFFSET(map_str),     AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "map_array",   "", OFFSET(map),         AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { "is_activate", "", OFFSET(running),     AV_OPT_TYPE_BOOL,        {.i64 = 0}, 0, 1, .flags = A|R },
    { NULL },
};

static const AVClass anxsrc_class = {
    .class_name          = "anxsrc_class",
    .item_name           = av_default_item_name,
    .option              = anxsrc_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_asrc_anxsrc = {
    .name            = "anxsrc",
    .description     = NULL_IF_CONFIG_SMALL("audio nuttx source(only pcm)"),
    .priv_size       = sizeof(ANxSrcPriv),
    .priv_class      = &anxsrc_class,
    .init            = anxsrc_init_dict,
    .uninit          = anxsrc_uninit,
    FILTER_QUERY_FUNC2(anxsrc_query_formats),
    .activate        = anxsrc_activate,
    .process_command = anxsrc_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
