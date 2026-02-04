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
#include "libavutil/frame.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "volume.h"
#include "mapping.h"

#include "filters.h"
#include "formats.h"

#define FADE_NONE 0
#define FADE_IN 1
#define FADE_OUT 2
#define FADE_OUT_IN (FADE_OUT|FADE_IN)

typedef struct BuffSrcPriv {
    const AVClass *class;
    char *map_str;
    int *map;
    /* nb_outputs needs to follow map because av_opt_get_array
       assumes the next address of map points to nb_outputs.*/
    int nb_outputs;
    enum PrecisionType precision;
    bool paused;

    int sample_rate;                /**< sample rate */
    AVChannelLayout ch_layout;      /**< channel layout */
    enum AVSampleFormat sample_fmt; /**< sample format */

    int fade_type;                  /**< fade type */
    AVFrame *frame;                 /**< frame buffer for fade. */
    int64_t next_pts;               /**< next expected pts for current input. */
    void (*fade_samples)(uint8_t **dst, uint8_t * const *src,
                        int nb_samples,int channels, int dir,
                        int64_t start, int64_t range);  /**< fade function */

    int (*on_event_cb)(void *udata, int evt, int64_t args);
    void *on_event_cb_udata;
    VolumeContext *vol_ctx;
    double player_volume;
    double *volume;
} BuffSrcPriv;

static void abufsrc_set_event_cb(AVFilterContext *ctx,
    int (*on_event_cb)(void *udata, int evt, int64_t args), void *udata)
{
    BuffSrcPriv *priv = ctx->priv;
    int i;

    priv->on_event_cb = on_event_cb;
    priv->on_event_cb_udata = udata;

    if (priv->on_event_cb) {
        for (i = 0; i < ctx->nb_outputs; i++) {
            if (priv->map && priv->map[i] == ROUTE_ON) {
                FilterLinkInternal *li = ff_link_internal(ctx->outputs[i]);
                li->frame_wanted_out = 1;
            }
        }

        ff_filter_set_ready(ctx, 100);
    }
}

static int abufsrc_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    BuffSrcPriv *priv = ctx->priv;
    AVFrame *clone;
    int i, ret = 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        clone = av_frame_clone(frame);
        if (!clone) {
            av_frame_free(&frame);
            return AVERROR(ENOMEM);
        }

        volume_scale(&priv->vol_ctx[i], clone);

        ret = ff_filter_frame(ctx->outputs[i], clone);
    }

    av_frame_free(&frame);
    return ret;
}

#define FADE(name, type)                                                                \
static void fade_samples_## name(uint8_t **dst, uint8_t * const *src, int nb_samples,   \
                                int channels, int dir, int64_t start, int64_t range)    \
{                                                                                       \
    type *d = (type *)dst[0];                                                           \
    const type *s = (type *)src[0];                                                     \
    int i, c, k = 0;                                                                    \
                                                                                        \
    for (i = 0; i < nb_samples; i++) {                                                  \
        double gain = av_clipd(1.0 * (start + i * dir) / range, 0, 1.0);                \
    for (c = 0; c < channels; c++, k++)                                                 \
        d[k] = s[k] * gain;                                                             \
    }                                                                                   \
}                                                                                       \

#define FADE_PLANAR(name, type)                                                             \
static void fade_samples_## name ##p(uint8_t **dst, uint8_t * const *src, int nb_samples,   \
                                    int channels, int dir, int64_t start, int64_t range)    \
{                                                                                           \
    int i, c;                                                                               \
                                                                                            \
    for (i = 0; i < nb_samples; i++) {                                                      \
        double gain = av_clipd(1.0 * (start + i * dir) / range, 0, 1.0);                    \
        for (c = 0; c < channels; c++) {                                                    \
            type *d = (type *)dst[c];                                                       \
            const type *s = (type *)src[c];                                                 \
            d[i] = s[i] * gain;                                                             \
        }                                                                                   \
    }                                                                                       \
}                                                                                           \


FADE_PLANAR(dbl, double)
FADE_PLANAR(flt, float)
FADE_PLANAR(s16, int16_t)
FADE_PLANAR(s32, int32_t)

FADE(dbl, double)
FADE(flt, float)
FADE(s16, int16_t)
FADE(s32, int32_t)

static void fade_frame(BuffSrcPriv* priv, int fade_type, AVFrame *dst, AVFrame *src)
{
    switch (src->format) {
        case AV_SAMPLE_FMT_S16:  priv->fade_samples = fade_samples_s16;  break;
        case AV_SAMPLE_FMT_S16P: priv->fade_samples = fade_samples_s16p; break;
        case AV_SAMPLE_FMT_S32:  priv->fade_samples = fade_samples_s32;  break;
        case AV_SAMPLE_FMT_S32P: priv->fade_samples = fade_samples_s32p; break;
        case AV_SAMPLE_FMT_FLT:  priv->fade_samples = fade_samples_flt;  break;
        case AV_SAMPLE_FMT_FLTP: priv->fade_samples = fade_samples_fltp; break;
        case AV_SAMPLE_FMT_DBL:  priv->fade_samples = fade_samples_dbl;  break;
        case AV_SAMPLE_FMT_DBLP: priv->fade_samples = fade_samples_dblp; break;
    }

    priv->fade_samples(dst->extended_data, src->extended_data, src->nb_samples,
                      src->ch_layout.nb_channels, fade_type > 1 ? -1 : 1,
                      fade_type > 1 ? src->nb_samples : 0, src->nb_samples);
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

        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    priv->player_volume = 1.0f;

    if (priv->map_str) {
        ret = avfilter_parse_mapping(priv->map_str, &priv->map, priv->nb_outputs);
        if (ret < 0)
            return ret;
    }

    priv->vol_ctx = av_calloc(priv->nb_outputs, sizeof(*priv->vol_ctx));
    if (!priv->vol_ctx) {
        av_freep(&priv->map);
        return AVERROR(ENOMEM);;
    }

    priv->volume = av_malloc(sizeof(*priv->volume) * priv->nb_outputs);
    if (!priv->volume) {
        av_freep(&priv->map);
        av_freep(&priv->vol_ctx);
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < priv->nb_outputs; i++)
        priv->volume[i] = 1.0f;

    return ret;
}

static av_cold void abufsrc_uninit(AVFilterContext *ctx)
{
    BuffSrcPriv *priv = ctx->priv;

    if (!priv)
        return;

    av_frame_free(&priv->frame);
    for (int i = 0; i < priv->nb_outputs; i++)
        volume_uninit(&priv->vol_ctx[i]);
    av_freep(&priv->vol_ctx);
    av_freep(&priv->map);
}

static int abufsrc_query_formats(const AVFilterContext *ctx,
                                 AVFilterFormatsConfig **cfg_in,
                                 AVFilterFormatsConfig **cfg_out)
{
    BuffSrcPriv *src = ctx->priv;
    int ret = 0;

    for (int i = 0; i < ctx->nb_outputs; i++) {
        AVFilterFormats *formats = NULL;
        AVFilterChannelLayouts *layouts = NULL;
        int fmts[] = { src->sample_fmt, -1 };
        int rates[] = { src->sample_rate, -1 };

        formats = ff_make_format_list(fmts);

        if (!formats) {
            ret = AVERROR(ENOMEM);
            goto out;
        }
        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(formats, &cfg_out[i]->formats);
        if (ret < 0)
            goto out;

        formats = ff_make_format_list(rates);
        if (!formats) {
            ret = AVERROR(ENOMEM);
            goto out;
        }
        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(formats, &cfg_out[i]->samplerates);
        if (ret < 0)
            goto out;

        ret = ff_add_channel_layout(&layouts, &src->ch_layout);
        if (ret < 0)
            goto out;

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
    FilterLinkInternal *li;
    int ret, routed = 0;
    AVFrame *frame;

    if (!priv->on_event_cb)
        return FFERROR_NOT_READY;

    for (int i = 0; i < priv->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_ON) {
            li = ff_link_internal(ctx->outputs[i]);
            if (li->frame_wanted_out) {
                routed = 1;
                if (priv->paused && li->frame_blocked_in == 0) {
                    li->frame_blocked_in = 1;
                    av_log(ctx, AV_LOG_INFO, "%s xrun\n", ctx->name);
                    ff_filter_set_ready(ctx->outputs[i]->dst, 300);
                }
            }
        }
    }

    if (!routed || priv->paused)
        return 0;

    if (!priv->frame) {
        priv->frame = av_frame_alloc();
        if (!priv->frame)
            return AVERROR(ENOMEM);

        if (ret = priv->on_event_cb(priv->on_event_cb_udata, 0, (intptr_t)priv->frame) < 0) {
            av_frame_free(&priv->frame);
            return ret;
        }

        priv->fade_type = FADE_IN;
        ff_filter_set_ready(ctx, 100);
        return 0;
    }

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    av_frame_move_ref(frame, priv->frame);
    if (priv->on_event_cb(priv->on_event_cb_udata, 0, (intptr_t)priv->frame) < 0) {
       av_frame_free(&priv->frame);
       priv->fade_type = FADE_OUT;
    }

    if (priv->next_pts == frame->pts && priv->fade_type == FADE_NONE) { //should not set fade again, when in fade process.
        int64_t next_pts = frame->pts + av_rescale_q(frame->nb_samples, (AVRational){1, frame->sample_rate}, frame->time_base);
        if (next_pts != priv->frame->pts)
            priv->fade_type = FADE_OUT_IN;
    }

    /* Do fade and clear fade flags.
     *
     * If fade out and fade in set at the same time, fade out should be done first
     * and fade in done in next frame.
     * If playing complete, next_pts will accumulate frame->nb_samples until next unsilent frame.
     */
    if (priv->fade_type) {
        if (priv->fade_type & FADE_OUT) {
            fade_frame(priv, FADE_OUT, frame, frame);
            priv->fade_type &= ~FADE_OUT;
        } else if (priv->fade_type & FADE_IN) {
            fade_frame(priv, FADE_IN, frame, frame);
            priv->fade_type &= ~FADE_IN;
        }
        priv->next_pts = frame->pts + av_rescale_q(frame->nb_samples, (AVRational){1, frame->sample_rate}, frame->time_base);
    } else { //if no fade occur during playing, next_pts should add frame->nb_samples.
        priv->next_pts += av_rescale_q(frame->nb_samples, (AVRational){1, frame->sample_rate}, frame->time_base);
    }

    return abufsrc_send_frame(ctx, frame);
}

static int abufsrc_fadeout_last_frame(AVFilterContext *ctx)
{
    BuffSrcPriv *priv = ctx->priv;
    AVFrame *frame = NULL;

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    av_frame_move_ref(frame, priv->frame);
    av_frame_free(&priv->frame);

    fade_frame(priv, FADE_OUT, frame, frame);

    priv->fade_type = FADE_NONE;

    return abufsrc_send_frame(ctx, frame);
}

static int abufsrc_set_output_volume(AVFilterContext *ctx, int index, double volume)
{
    BuffSrcPriv *priv = ctx->priv;
    int i;

    if (index < -1 || index >= (int)ctx->nb_outputs) {
        av_log(ctx, AV_LOG_ERROR, "Invalid index: %d\n", index);
        return AVERROR(EINVAL);
    }

    if (index == -1) {
        for (i = 0; i < ctx->nb_outputs; i++) {
            priv->volume[i] = volume;
            volume_set(&priv->vol_ctx[i], volume);
        }
        return 0;
    }

    priv->volume[index] = volume;
    volume_set(&priv->vol_ctx[index], volume);

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
            av_log(ctx, AV_LOG_ERROR, "Unable to parse '%s': %s\n", p, av_err2str(ret));
            break;
        }
        if (*p)
            p++;
        av_log(ctx, AV_LOG_INFO, "Parsed Key: %s, Value: %s\n", key, value);
        if (!strcmp(key, "player_volume")) {
            priv->player_volume = strtof(value, NULL);
            for (int i = 0; i < ctx->nb_outputs; i++)
                volume_set(&priv->vol_ctx[i], priv->player_volume * priv->volume[i]);
        } else if (!strcmp(key, "volume")) {
            double volume;
            int index;

            ret = volume_parse_index_db(value, &index, &volume);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error when parsing %s volume expression '%s'\n",
                       ctx->name, value);
                goto end;
            }

            abufsrc_set_output_volume(ctx, index, volume);
        } else
            av_log(ctx, AV_LOG_ERROR, "Unknown parameter: %s\n", key);

end:
        av_freep(&key);
        av_freep(&value);
    }
    return ret;
}

static int abufsrc_get_latency(AVFilterContext *ctx, int64_t *latency)
{
    BuffSrcPriv *priv = ctx->priv;
    int64_t sink_latency = 0;
    int64_t src_latency = 0;
    char msg[64];
    int ret = 0;
    int i;

    if (priv->frame)
        src_latency = av_rescale_q(priv->frame->duration, priv->frame->time_base, AV_TIME_BASE_Q);

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->map[i] == ROUTE_ON) {
            if (ff_outlink_get_status(ctx->outputs[i])) {
                if (latency)
                    *latency = -1;
                return AVERROR(EINVAL);
            }
            ret = avfilter_forward_command(ctx, i, NULL, "latency", NULL, (char*)&sink_latency, sizeof(sink_latency), 0);
            if (ret < 0)
                return ret;
        }
    }

    *latency = src_latency + sink_latency;

    av_log(ctx, AV_LOG_INFO, "src_latency %" PRId64 " sink_latency %" PRId64, src_latency, sink_latency);

    return ret;
}

static int abufsrc_dump_info(AVFilterContext *ctx, char *res, int res_len)
{
    BuffSrcPriv *priv = ctx->priv;
    int offset = 0;
    int ret;
    int i;

    if (!res || res_len <= 0 || !priv->volume || !priv->map)
        return AVERROR(EINVAL);

    ret = snprintf(res + offset, res_len - offset, "player_volume:%.2f volume:", priv->player_volume);
    if (ret > 0)
        offset = FFMIN(offset + ret, res_len - 1);

    for (i = 0; i < ctx->nb_outputs && offset < res_len - 1; i++) {
        ret = snprintf(res + offset, res_len - offset, " %.2f", priv->volume[i]);
        if (ret > 0)
            offset = FFMIN(offset + ret, res_len - 1);
    }

    ret = snprintf(res + offset, res_len - offset, " map:");
    if (ret > 0)
        offset = FFMIN(offset + ret, res_len - 1);

    for (i = 0; i < ctx->nb_outputs && offset < res_len - 1; i++) {
        ret = snprintf(res + offset, res_len - offset, " %d", priv->map[i]);
        if (ret > 0)
            offset = FFMIN(offset + ret, res_len - 1);
    }

    return 0;
}

static int abufsrc_get_parameter(AVFilterContext *ctx, const char *key, char *value, int len)
{
    BuffSrcPriv *s = ctx->priv;
    int64_t latency;
    int ret = 0;

    if (!strcmp(key, "format")) {
        snprintf(value, len, "fmt=%d:rate=%d:ch=%d", s->sample_fmt, s->sample_rate, s->ch_layout.nb_channels);
        return 0;
    } else if (!strcmp(key, "player_volume")) {
        snprintf(value, len, "vol:%f", s->player_volume);

        av_log(s, AV_LOG_INFO, "get_parameter: %s = %.2f\n", key, s->player_volume);
        return 0;
    } else if (!strncmp(key, "volume", 6)) {
        char *index_str = NULL;
        char *parsed_key = NULL;
        int idx = -1;

        int ret = av_opt_get_key_value(&key, "=", ":", 0, &parsed_key, &index_str);
        if (!index_str || ret < 0)
            idx = 0;
        else {
            idx = (int)strtol(index_str, NULL, 0);
            if (idx < -1 || idx >= ctx->nb_outputs) {
                av_log(s, AV_LOG_ERROR, "Invalid volume index: %s\n", index_str);
                av_freep(&index_str);
                av_freep(&parsed_key);
                return AVERROR(EINVAL);
            }
        }
        snprintf(value, len, "vol:%f", s->volume[idx]);
        av_log(s, AV_LOG_INFO, "get_parameter: volume[%d] = %.2f\n", idx, s->volume[idx]);
        av_freep(&index_str);
        av_freep(&parsed_key);
        return 0;
    } else if (!strcmp(key, "latency")) {
        ret = abufsrc_get_latency(ctx, &latency);
        snprintf(value, len, "latency:%" PRId64, latency);

        av_log(s, AV_LOG_INFO, "get_parameter: %s = %" PRId64 "\n", key, latency);
        return ret;
    }

    av_log(ctx, AV_LOG_ERROR, "get_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

static int abufsrc_proccess_command(AVFilterContext *ctx, const char *cmd, const char *args,
    char *res, int res_len, int flags)
{
    BuffSrcPriv *priv = ctx->priv;
    int ret = 0;
    int i;

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

        priv->next_pts = 0;
        priv->paused = false;

        priv->sample_fmt = format;
        priv->sample_rate = sample_rate;
        av_channel_layout_default(&priv->ch_layout, channels);

        abufsrc_set_event_cb(ctx, on_event_cb, udata);

        for (i = 0; i < ctx->nb_outputs; i++) {
            if (priv->map[i] == ROUTE_ON) {
                avfilter_forward_command(ctx, i, NULL, "play", NULL, NULL, 0, 0);
            }

            ret = volume_init(&priv->vol_ctx[i], format, priv->precision);
            volume_set(&priv->vol_ctx[i], priv->player_volume * priv->volume[i]);
        }

        return ret;
    } else if (!av_strcasecmp(cmd, "unlink")) {
        if (priv->frame)
            ret = abufsrc_fadeout_last_frame(ctx);

        if (priv->on_event_cb)
            priv->on_event_cb(priv->on_event_cb_udata, -1, 0);

        for (i= 0; i < priv->nb_outputs; i++) {
            if (priv->map && priv->map[i] == ROUTE_ON) {
                ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
                avfilter_forward_command(ctx, i, NULL, "pause", NULL, NULL, 0, 0);
            }

            volume_uninit(&priv->vol_ctx[i]);
        }

        priv->sample_fmt = AV_SAMPLE_FMT_NONE;
        priv->sample_rate = 0;
        av_channel_layout_uninit(&priv->ch_layout);

        abufsrc_set_event_cb(ctx, NULL, NULL);


        return ret;
    } else if (!av_strcasecmp(cmd, "map")) {
        int *old_map = NULL;

        if (!priv->map)
            return AVERROR(EINVAL);

        old_map = av_calloc(priv->nb_outputs, sizeof(*old_map));
        if (!old_map)
            return AVERROR(ENOMEM);

        memcpy(old_map, priv->map, priv->nb_outputs * sizeof(*old_map));

        ret = avfilter_parse_mapping(args, &priv->map, priv->nb_outputs);
        if (ret < 0) {
            av_freep(&old_map);
            return ret;
        }

        for (i = 0; i < priv->nb_outputs && old_map; i++) {
            if (old_map[i] != priv->map[i]) {
                if (old_map[i] == ROUTE_ON && priv->map[i] == ROUTE_OFF) {
                    ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
                    avfilter_forward_command(ctx, i, NULL, "pause", NULL, NULL, 0, 0);
                } else if (old_map[i] == ROUTE_OFF && priv->map[i] == ROUTE_ON) {
                    FilterLinkInternal *li = ff_link_internal(ctx->outputs[i]);
                    li->frame_wanted_out = 1;
                    if (!ff_outlink_get_status(ctx->outputs[i]))
                        avfilter_forward_command(ctx, i, NULL, "play", NULL, NULL, 0, 0);
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
    } else if (!av_strcasecmp(cmd, "pause")) {
        priv->paused = true;
        if (priv->frame)
            ret = abufsrc_fadeout_last_frame(ctx);

        for (i = 0; i < ctx->nb_outputs; i++) {
            if (priv->map && priv->map[i] == ROUTE_ON)
                avfilter_forward_command(ctx, i, NULL, "pause", NULL, NULL, 0, 0);
        }

        return 0;
    } else if (!av_strcasecmp(cmd, "resume")) {
        priv->paused = false;
        for (i = 0; i < ctx->nb_outputs; i++) {
            if (priv->map && priv->map[i] == ROUTE_ON)
                avfilter_forward_command(ctx, i, NULL, "play", NULL, NULL, 0, 0);
        }

        ff_filter_set_ready(ctx, 100);
        return 0;
    } else if (!av_strcasecmp(cmd, "dump")) {
        return abufsrc_dump_info(ctx, res, res_len);
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
    { "precision", "select mathematical precision",
            OFFSET(precision), AV_OPT_TYPE_INT, { .i64 = PRECISION_FIXED }, PRECISION_FIXED, PRECISION_DOUBLE, A|F, "precision" },
        { "fixed",  "select 8-bit fixed-point",     0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_FIXED  }, INT_MIN, INT_MAX, A|F, "precision" },
        { "float",  "select 32-bit floating-point", 0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_FLOAT  }, INT_MIN, INT_MAX, A|F, "precision" },
        { "double", "select 64-bit floating-point", 0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_DOUBLE }, INT_MIN, INT_MAX, A|F, "precision" },
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
    FILTER_QUERY_FUNC2(abufsrc_query_formats),
    .activate        = abufsrc_activate,
    .process_command = abufsrc_proccess_command,
    .flags           = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
