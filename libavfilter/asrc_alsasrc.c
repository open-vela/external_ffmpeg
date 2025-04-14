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
 * audio alsa src
 */

#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include "libavutil/time.h"

#include <poll.h>

#include "alsa.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"
#include "aresample.h"
#include "libavutil/mem.h"

#define ROUTE_OFF 0
#define ROUTE_ON 1

typedef struct AlsasrcPriv {
    const AVClass *class;

    AlsaHandle priv;

    int format_id;
    char *devname;

    uint32_t sample_rate;
    AVChannelLayout ch_layout;

    int periods;
    int period_time;
    int period_size;
    int frame_size;

    char *map_str;
    int *map;
    int nb_outputs;

    int64_t timestamp;
    AVPacket *pkt;
} AlsasrcPriv;

static inline void alsasrc_force_request(AVFilterContext *ctx)
{
    AlsasrcPriv *s = ctx->priv;
    FilterLinkInternal *li;
    int i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (s->map && s->map[i] == ROUTE_OFF)
            continue;

        li = ff_link_internal(ctx->outputs[i]);
        li->frame_wanted_out = 1;
        ff_filter_set_ready(ctx, 300);
    }
}

static int alsasrc_get_device_support_format(AVFilterContext *ctx, const char *devname, const char *key, int value, int *out_value)
{
    AVOptionRanges* ranges = NULL;
    AVOptionRange* range = NULL;
    int ret, range_idx;

    ret = alsa_query_caps(&ranges, devname, key, false);
    if (ret > 0) {
        for (range_idx = 0; range_idx < ranges->nb_ranges; range_idx++) {
            range = ranges->range[range_idx];
            if (value >= range->value_min && value <= range->value_max)
                break;
        }

        *out_value = range_idx == ranges->nb_ranges ? range->value_min : value;
        av_opt_freep_ranges(&ranges);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Unsupported query %s: %d\n", key, value);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int alsasrc_config_formats(AVFilterLink *link, int pad)
{
    // TODO: optimize
    AVFilterContext *ctx = link->src;
    AlsasrcPriv *priv = ctx->priv;
    int ret;

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_fmts", link->format, &priv->format_id);
    if (ret < 0)
        return ret;

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_rates", link->sample_rate, &priv->sample_rate);
    if (ret < 0)
        return ret;

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "channels", link->ch_layout.nb_channels, &priv->ch_layout.nb_channels);
    if (ret < 0)
        return ret;

    if (priv->ch_layout.nb_channels == link->ch_layout.nb_channels)
        av_channel_layout_copy(&priv->ch_layout, &link->ch_layout);
    else
        av_channel_layout_default(&priv->ch_layout, priv->ch_layout.nb_channels);

    return 0;
}

static int alsasrc_config_props(AVFilterLink *link)
{
    av_log(link->src, AV_LOG_INFO, "link sample format: %s, sample_rate %d, channels %d.\n",
        av_get_sample_fmt_name(link->format), link->sample_rate,
        link->ch_layout.nb_channels);
    return 0;
}

static void alsasrc_close(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;

    alsa_close(&priv->priv);

    if (priv->pkt != NULL) {
        av_packet_free(&priv->pkt);
    }
}

static int alsasrc_init_dict(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    int i, ret;

    for (i = 0; i < priv->nb_outputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        pad.config_props = alsasrc_config_props;
        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    if (priv->map_str) {
        ret = avfilter_parse_mapping(priv->map_str, &priv->map, priv->nb_outputs);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void alsasrc_uninit(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    int i;

    av_freep(&priv->map);
}

static int alsasrc_query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in, AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    int ret, i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
        ff_formats_unref(&cfg_in[i]->formats);
        ret = ff_formats_ref(formats, &cfg_in[i]->formats);
        if (ret < 0)
            goto out;

        formats = ff_all_samplerates();
        ff_formats_unref(&cfg_in[i]->samplerates);
        ret = ff_formats_ref(formats, &cfg_in[i]->samplerates);
        if (ret < 0)
            goto out;

        layouts = ff_all_channel_counts();
        ff_channel_layouts_unref(&cfg_in[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_in[i]->channel_layouts);
        if (ret < 0)
            goto out;
    }

out:
    return ret;
}

static void alsasrc_set_eof(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    int i;

    for (i = 0; i < ctx->nb_outputs; i++)
        ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
}

static int alsasrc_check_outlink_status(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int need_close = 1;
    int i, ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ff_outlink_get_status(ctx->outputs[i]) != AVERROR_EOF) {
            need_close = 0;
            break;
        }
    }

    if (need_close && handle->h) {
        alsasrc_close(ctx);
        return 0;
    }

    return 1;
}

static int alsasrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int ret;

    if (!strcmp(cmd, "link")) {
        alsasrc_force_request(ctx);
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        if (handle->h) {
            alsasrc_close(ctx);
            alsasrc_set_eof(ctx);
        }

        return 0;
    } else if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll = (struct pollfd *)res;
        int ret;

        if (!res || res_len < sizeof(struct pollfd))
            return AVERROR(EINVAL);

        if (!handle->h)
            return 0;

        ret = snd_pcm_poll_descriptors(handle->h, poll, 1);

        if (ret < 0)
            return 0;

        return 1;
    } else if (!strcmp(cmd, "poll_available")) {
        ff_filter_set_ready(ctx, 100);
        return 0;
    } else if (!strcmp(cmd, "map")) {
        ret = avfilter_parse_mapping(args, &priv->map, priv->nb_outputs);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to parse mapping: %s ret:%d\n", args, ret);
            return ret;
        }

        for (int i = 0; i < ctx->nb_outputs; i++) {
            AVFilterLink *link = ctx->outputs[i];
            if (priv->map && priv->map[i] == ROUTE_OFF)
            {
                av_log(ctx, AV_LOG_INFO, "disable output%d\n", i);
                ff_inlink_set_status(link, AVERROR_EOF);
            }
        }

        ff_filter_set_ready(ctx, 100);
        return ret;
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int alsasrc_read_packet(AlsasrcPriv *priv)
{
    AlsaHandle *handle = &priv->priv;
    int res;

    res = alsa_read(handle, priv->pkt->data, priv->period_size);
    if (res < 0)
        return res;

    priv->pkt->size = res * handle->frame_size;
    priv->pkt->pts = av_rescale(priv->timestamp, 1000000, priv->sample_rate);
    priv->timestamp += res;

    return res;
}

static int alsasrc_open(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int ret;
    int i;

    if (handle->h)
        return 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        alsasrc_config_formats(ctx->outputs[i], i);
        break;
    }

    priv->period_size = priv->period_time * priv->sample_rate / 1000;
    handle->periods = priv->periods;
    handle->period_time = priv->period_time;

    ret = alsa_open(handle, priv->devname, SND_PCM_STREAM_CAPTURE, priv->sample_rate, priv->ch_layout.nb_channels, priv->format_id);
    if (ret < 0)
        return ret;

    priv->pkt = av_packet_alloc();
    if (!priv->pkt) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    priv->pkt->size = priv->period_size * handle->frame_size;
    if (priv->pkt->size <= 0) {
        ret = AVERROR(EINVAL);
        goto error;
    }

    ret = av_new_packet(priv->pkt, priv->pkt->size);
    if (ret < 0) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    priv->timestamp = 0;

    return 0;

error:
    alsasrc_close(ctx);
    return ret;
}

static int alsasrc_wrap_frame(AVFilterContext *ctx, int pad, AVPacket **pkt, AVFrame **frame)
{
    AVFilterLink *link = ctx->outputs[pad];
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int sample_bytes;
    AVFrame *src;
    int ret;

    src = av_frame_alloc();
    if (!src) {
        return AVERROR(ENOMEM);
    }

    src->format = priv->format_id;
    src->sample_rate = priv->sample_rate;
    av_channel_layout_copy(&src->ch_layout, &priv->ch_layout);

    sample_bytes = handle->frame_size * priv->ch_layout.nb_channels;

    src->nb_samples = (*pkt)->size / sample_bytes;
    src->pkt_size = (*pkt)->size;

    src->buf[0] = (*pkt)->buf;
    src->data[0] = src->buf[0]->data;
    src->linesize[0] = (*pkt)->size;
    src->extended_data = src->data;
    src->pts = (*pkt)->pts;

    *frame = src;
    ret = 0;

    (*pkt)->buf = NULL;

    return ret;
}

static int alsasrc_activate(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int i, ret;

    ret = alsasrc_check_outlink_status(ctx);
    if (!ret)
        return ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ff_outlink_frame_wanted(ctx->outputs[i])) {
            ret = alsasrc_open(ctx);
            if (ret < 0)
                goto out;

            break;
        }
    }

    if (i == ctx->nb_outputs)
       return FFERROR_NOT_READY;

    ret = alsasrc_read_packet(priv);
    if (ret < 0)
        goto out;

    for (i = 0; i < ctx->nb_outputs; i++) {
        AVFrame *frame = NULL;
        AVFilterLink *link;
        AVPacket *pkt_out;

        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        pkt_out = av_packet_clone(priv->pkt);
        if (!pkt_out) {
            ret = AVERROR(ENOMEM);
            goto out;
        }

        ret = alsasrc_wrap_frame(ctx, i, &pkt_out, &frame);
        if (ret < 0)
            goto out;

        av_packet_free(&pkt_out);

        link = ctx->outputs[i];
        ret = ff_filter_frame(link, frame);
        if (ret < 0)
            goto out;
    }

out:
    return ret;
}

#define OFFSET(x) offsetof(AlsasrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption alsasrc_options[] = {
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = A },
    { "sample_rate", "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                  0, INT_MAX, R },
    { "ch_layout",   "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},               0, 0,       R },
    { "periods",     "", OFFSET(periods),     AV_OPT_TYPE_INT,        {.i64 = 4},                  0, INT_MAX, R },
    { "period_time", "", OFFSET(period_time), AV_OPT_TYPE_INT,        {.i64 = 20},                 0, INT_MAX, R },
    { "outputs",     "", OFFSET(nb_outputs),  AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "map",         "", OFFSET(map_str),     AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "map_array",   "", OFFSET(map),         AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { NULL },
};

static const AVClass alsasrc_class = {
    .class_name          = "alsasrc_class",
    .item_name           = av_default_item_name,
    .option              = alsasrc_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_asrc_alsasrc = {
    .name            = "alsasrc",
    .description     = NULL_IF_CONFIG_SMALL("Audio alsa src"),
    .priv_class      = &alsasrc_class,
    .priv_size       = sizeof(AlsasrcPriv),
    .init            = alsasrc_init_dict,
    .uninit          = alsasrc_uninit,
    FILTER_QUERY_FUNC2(alsasrc_query_formats),
    .activate        = alsasrc_activate,
    .process_command = alsasrc_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
