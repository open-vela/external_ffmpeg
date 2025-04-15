/*
 * Copyright (c) 2024 HiccupZhu
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
 * memory buffer source filter
 */

#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"
#include "aresample.h"
#include "volume.h"

#define SUGGESTED_NB_SAMPLES 1024

#define ROUTE_ON 1
#define ROUTE_OFF 0

typedef struct BuffSrcPriv {
    const AVClass *class;
    char *map_str;
    int *map;
    /* nb_outputs needs to follow map because av_opt_get_array
       assumes the next address of map points to nb_outputs.*/
    int nb_outputs;

    int sample_rate;            /**< sample rate */
    AVChannelLayout ch_layout;  /**< channel layout */
    enum AVSampleFormat sample_fmt;  /**< sample format */

    int (*on_event_cb)(void *udata, int evt, int64_t args);
    void *on_event_cb_udata;
    VolumeContext vol_ctx;
    double player_volume;
    double stream_volume;
} BuffSrcPriv;

static int attribute_align_arg abufsrc_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    BuffSrcPriv *priv = ctx->priv;
    int i, ret, first = 1;

    volume_scale(&priv->vol_ctx, frame);

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        if (first) { // do not clone at fisrt sending.
            ret = ff_filter_frame(ctx->outputs[i], frame);
            if (ret < 0)
                return ret;
            first = 0;
        } else {
            AVFrame *clone = av_frame_clone(frame);
            if (!clone)
                return AVERROR(ENOMEM);

            ret = ff_filter_frame(ctx->outputs[i], clone);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static int av_cold abufsrc_set_event_cb(AVFilterContext *ctx,
    int (*on_event_cb)(void *udata, int evt, int64_t args), void *udata)
{
    BuffSrcPriv *priv = ctx->priv;
    int i, ret;

    priv->on_event_cb = on_event_cb;
    priv->on_event_cb_udata = udata;

    if (priv->on_event_cb) {
        for (i = 0; i < ctx->nb_outputs; i++) {
            FilterLinkInternal *li = ff_link_internal(ctx->outputs[i]);
            li->frame_wanted_out = 1;
        }

        ff_filter_set_ready(ctx, 100);
    }

    return 0;
}

static int config_props(AVFilterLink *link)
{
    return 0;
}

static av_cold int abufsrc_init_dict(AVFilterContext *ctx)
{
    BuffSrcPriv *priv = ctx->priv;
    int i, ret = 0;

    for (i = 0; i < priv->nb_outputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        pad.config_props = config_props;
        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    priv->player_volume = 1.0f;
    priv->stream_volume = 1.0f;

    if (priv->map_str) {
        ret = avfilter_parse_mapping(priv->map_str, &priv->map, priv->nb_outputs);
        if (ret < 0)
            return ret;
    }

    return ret;
}

static av_cold void abufsrc_uninit(AVFilterContext *ctx)
{
    BuffSrcPriv *priv = ctx->priv;
    av_freep(&priv->map);
}

static int abufsrc_query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    int ret, i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(formats, &cfg_out[i]->formats);
        if (ret < 0)
            goto out;


        formats = ff_all_samplerates();
        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(formats, &cfg_out[i]->samplerates);
        if (ret < 0)
            goto out;

        layouts = ff_all_channel_counts();
        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_out[i]->channel_layouts);
        if (ret < 0)
            goto out;
    }

out:
    return ret;
}

static int abufsrc_activate(AVFilterContext *ctx)
{
    BuffSrcPriv *priv = ctx->priv;
    AVFrame *frame;
    int i, ret, routed = 0;

    for (i = 0; i < priv->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_ON) {
            routed = 1;
            if (!ff_outlink_frame_wanted(ctx->outputs[i]))
                return 0;
        }
    }

    if (!routed)
        return 0;

    if (!priv->on_event_cb)
        return 0;

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    frame->nb_samples = SUGGESTED_NB_SAMPLES;
    ret = priv->on_event_cb(priv->on_event_cb_udata, 0, (intptr_t)frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    ret = abufsrc_send_frame(ctx, frame);
    if (ret < 0)
        return ret;

    return 0;
}

static int abufsrc_set_parameter(AVFilterContext *ctx, const char *args)
{

    BuffSrcPriv *priv = ctx->priv;
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
        if (!strcmp(key, "volume")) {
            priv->player_volume = strtof(value, NULL);
            volume_set(&priv->vol_ctx, priv->player_volume * priv->stream_volume);
        } else if (!strcmp(key, "stream_volume")){
            priv->stream_volume = strtof(value, NULL);
            volume_set(&priv->vol_ctx, priv->player_volume * priv->stream_volume);
        } else
            av_log(ctx, AV_LOG_ERROR, "Unknown parameter: %s\n", key);
        av_freep(&key);
        av_freep(&value);
    }
    return ret;
}

static int abufsrc_get_parameter(AVFilterContext *ctx, const char *key, char *value, int len)
{
    BuffSrcPriv *s = ctx->priv;

    if (!strcmp(key, "format")) {
        snprintf(value, len, "fmt=%d:rate=%d:ch=%d", s->sample_fmt, s->sample_rate, s->ch_layout.nb_channels);
        return 0;
    }

    av_log(ctx, AV_LOG_ERROR, "get_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

static int abufsrc_proccess_command(AVFilterContext *ctx, const char *cmd, const char *args,
    char *res, int res_len, int flags)
{
    BuffSrcPriv *priv = ctx->priv;
    int ret;

    if (!cmd)
        return AVERROR(EINVAL);

    av_log(ctx, AV_LOG_INFO, "cmd:%s args:%s\n", cmd, args);
    if (!av_strcasecmp(cmd, "link")) {
        int (*on_event_cb)(void *udata, int evt, int64_t args);
        int format, sample_rate, channels;
        void *udata;

        if (!args)
            return AVERROR(EINVAL);

        if (sscanf(args, "%p %p fmt=%d:rate=%d:ch=%d", &on_event_cb, &udata, &format, &sample_rate, &channels) != 5)
            return AVERROR(EINVAL);

        priv->sample_fmt = format;
        priv->sample_rate = sample_rate;
        av_channel_layout_default(&priv->ch_layout, channels);

        ret = abufsrc_set_event_cb(ctx, on_event_cb, udata);
        if (ret < 0)
            return ret;

        ret = volume_init(&priv->vol_ctx, format);
        return ret;
    } else if (!av_strcasecmp(cmd, "unlink")) {
        int i;

        for (i= 0; i < priv->nb_outputs; i++)
             ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);

        if (priv->on_event_cb)
            priv->on_event_cb(priv->on_event_cb_udata, -1, 0);

        volume_uninit(&priv->vol_ctx);
        ret = abufsrc_set_event_cb(ctx, NULL, NULL);
        return ret;
    } else if (!av_strcasecmp(cmd, "map")) {
        int *old_map = NULL;
        int i;

        if (priv->map) {
            old_map = av_calloc(priv->nb_outputs, sizeof(*old_map));
            if (!old_map)
                return AVERROR(ENOMEM);

            memcpy(old_map, priv->map, priv->nb_outputs * sizeof(*old_map));
        }

        ret = avfilter_parse_mapping(args, &priv->map, priv->nb_outputs);
        if (ret < 0) {
            av_freep(&old_map);
            return ret;
        }

        for (i = 0; i < priv->nb_outputs; i++) {
            if (old_map[i] != priv->map[i]) {
                if (old_map[i] == ROUTE_ON && priv->map[i] == ROUTE_OFF) {
                    ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
                } else if (old_map[i] == ROUTE_OFF && priv->map[i] == ROUTE_ON) {
                    FilterLinkInternal *li = ff_link_internal(ctx->outputs[i]);
                    li->frame_wanted_out = 1;
                }
            }
        }

        av_freep(&old_map);
        ff_filter_set_ready(ctx, 100);
        return ret;
    } else if (!av_strcasecmp(cmd, "get_parameter")) {
        if (!args || res_len <= 0)
            return AVERROR(EINVAL);

        return abufsrc_get_parameter(ctx, args, res, res_len);
    } else if (!av_strcasecmp(cmd, "set_parameter")) {
        if (!args)
            return AVERROR(EINVAL);

        return abufsrc_set_parameter(ctx, args);
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

#define OFFSET(x) offsetof(BuffSrcPriv, x)
#define A AV_OPT_FLAG_AUDIO_PARAM
#define F AV_OPT_FLAG_FILTERING_PARAM

static const AVOption abuffer_options[] = {
    { "outputs", "set number of outputs", OFFSET(nb_outputs), AV_OPT_TYPE_INT,   { .i64 = 1 }, 1, INT_MAX, A },
    { "map", "input indexes to remap to outputs", OFFSET(map_str),    AV_OPT_TYPE_STRING, {.str=NULL},    .flags = A|F },
    { "map_array", "get map list", OFFSET(map),    AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX,    .flags = A|F },
    { NULL },
};

AVFILTER_DEFINE_CLASS(abuffer);

const AVFilter ff_asrc_abufsrc = {
    .name            = "abufsrc",
    .description     = NULL_IF_CONFIG_SMALL("Buffer audio frames, and make them accessible to the filterchain."),
    .priv_size       = sizeof(BuffSrcPriv),
    .priv_class      = &abuffer_class,
    .init            = abufsrc_init_dict,
    .uninit          = abufsrc_uninit,
    .activate        = abufsrc_activate,
    FILTER_QUERY_FUNC2(abufsrc_query_formats),
    .process_command = abufsrc_proccess_command,
    .flags           = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
