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

#define ROUTE_ON 1
#define ROUTE_OFF 0

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
    VolumeContext vol_ctx;
    double player_volume;
    double volume;
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
            FilterLinkInternal *li = ff_link_internal(ctx->outputs[i]);
            li->frame_wanted_out = 1;
        }

        ff_filter_set_ready(ctx, 100);
    }
}

static int abufsrc_send_frame(AVFilterContext *ctx, AVFrame *frame)
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
    priv->volume = 1.0f;

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

static int abufsrc_activate(AVFilterContext *ctx)
{
    BuffSrcPriv *priv = ctx->priv;
    int i, ret, routed = 1;
    FilterLinkInternal *li;
    AVFrame *frame;

    if (!priv->on_event_cb)
        return FFERROR_NOT_READY;

    for (i = 0; i < priv->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_ON) {
            li = ff_link_internal(ctx->outputs[i]);
            if (li->frame_wanted_out) {
                if (priv->paused && li->frame_blocked_in == 0) {
                    li->frame_blocked_in = 1;
                    av_log(ctx, AV_LOG_INFO, "%s xrun\n", ctx->name);
                    ff_filter_set_ready(ctx->outputs[i]->dst, 300);
                }
            } else
                routed = 0;
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
        if (!strcmp(key, "player_volume")) {
            priv->player_volume = strtof(value, NULL);
            volume_set(&priv->vol_ctx, priv->player_volume * priv->volume);
        } else if (!strcmp(key, "volume")) {
            double volume;
            ret = av_expr_parse_and_eval(&volume, value, NULL, NULL, NULL, NULL,
                                         NULL, NULL, NULL, 0, NULL);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error when parsing %s volume expression '%s'\n",
                       ctx->name, value);
                goto end;
            }
            priv->volume = volume;
            volume_set(&priv->vol_ctx, priv->player_volume * priv->volume);
        } else
            av_log(ctx, AV_LOG_ERROR, "Unknown parameter: %s\n", key);

end:
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
    } else if (!strcmp(key, "player_volume")) {
        snprintf(value, len, "vol:%f", s->player_volume);

        av_log(s, AV_LOG_INFO, "get_parameter: %s = %.2f\n", key, s->player_volume);
        return 0;
    }

    av_log(ctx, AV_LOG_ERROR, "get_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

static int abufsrc_proccess_command(AVFilterContext *ctx, const char *cmd, const char *args,
    char *res, int res_len, int flags)
{
    BuffSrcPriv *priv = ctx->priv;
    int ret = 0;

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

        ret = volume_init(&priv->vol_ctx, format);
        volume_set(&priv->vol_ctx, priv->player_volume * priv->volume);
        return ret;
    } else if (!av_strcasecmp(cmd, "unlink")) {
        int i;

        if (priv->frame)
            ret = abufsrc_fadeout_last_frame(ctx);

        if (priv->on_event_cb)
            priv->on_event_cb(priv->on_event_cb_udata, -1, 0);

        for (i= 0; i < priv->nb_outputs; i++) {
            if (priv->map && priv->map[i] == ROUTE_ON)
                ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
        }

        priv->sample_fmt = AV_SAMPLE_FMT_NONE;
        priv->sample_rate = 0;
        av_channel_layout_uninit(&priv->ch_layout);

        abufsrc_set_event_cb(ctx, NULL, NULL);

        volume_uninit(&priv->vol_ctx);

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
    } else if (!av_strcasecmp(cmd, "pause")) {
        priv->paused = true;
        if (priv->frame)
            ret = abufsrc_fadeout_last_frame(ctx);
        return 0;
    } else if (!av_strcasecmp(cmd, "resume")) {
        priv->paused = false;
        ff_filter_set_ready(ctx, 100);
        return 0;
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
    .process_command = abufsrc_proccess_command,
    .flags           = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
