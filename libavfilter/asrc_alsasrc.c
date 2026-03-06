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

#include <libavutil/eval.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include "libavutil/time.h"
#include "libavutil/mem.h"

#include <poll.h>

#include "alsa.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"
#include "volume.h"
#include "mapping.h"

typedef struct AlsasrcPriv {
    const AVClass *class;

    AlsaHandle priv;
    char *devname;

    enum AVSampleFormat format;
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

    VolumeContext *vol_ctx;
    double *volume;
    enum PrecisionType precision;
    bool mute;
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

static int alsasrc_get_device_support_format(AVFilterContext *ctx, const char *devname,
                                             const char *key, int value, int *out_value)
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

        *out_value = range_idx == ranges->nb_ranges ? ranges->range[0]->value_min : value;
        av_opt_freep_ranges(&ranges);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Unsupported query %s: %d\n", key, value);
        return AVERROR(EINVAL);
    }

    return 0;
}

static void alsasrc_close(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int i;

    if (handle->h) {
        alsa_close(&priv->priv);
    }

    if (priv->vol_ctx) {
        for (i = 0; i < priv->nb_outputs; i++) {
            volume_uninit(&priv->vol_ctx[i]);
        }
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

        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    if (priv->map_str) {
        ret = avfilter_parse_mapping(priv->map_str, &priv->map, priv->nb_outputs);
        if (ret < 0)
            return ret;
    }

    priv->vol_ctx = av_calloc(priv->nb_outputs, sizeof(*priv->vol_ctx));
    if (!priv->vol_ctx) {
        av_freep(&priv->map);
        return AVERROR(ENOMEM);
    }

    priv->volume = av_malloc(sizeof(*priv->volume) * priv->nb_outputs);
    if (!priv->volume) {
        av_freep(&priv->map);
        av_freep(&priv->vol_ctx);
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < priv->nb_outputs; i++)
        priv->volume[i] = -1.0f;

    priv->mute = false;
    return 0;
}

static void alsasrc_uninit(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;

    alsasrc_close(ctx);

    av_freep(&priv->map);
    av_freep(&priv->vol_ctx);
    av_freep(&priv->volume);
}

static int alsasrc_query_formats(const AVFilterContext *ctx,
                                 AVFilterFormatsConfig **cfg_in,
                                 AVFilterFormatsConfig **cfg_out)
{
    AVFilterFormats *samprates = NULL, *fmts = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    AVOptionRanges *caps_ranges = NULL;
    AlsasrcPriv *priv = ctx->priv;
    AVChannelLayout layout;
    int n, i, j, ret;

    if (priv->sample_rate > 0) {
        ret = ff_add_format(&fmts, priv->sample_rate);
        if (ret < 0)
            goto out;
    } else {
        ret = alsa_query_caps(&caps_ranges, priv->devname, "sample_rates", false);
        if (ret >= 0) {
            for (n = 0; n < caps_ranges->nb_ranges; n++) {
                ret = ff_add_format(&fmts, caps_ranges->range[n]->value_min);
                if (ret < 0)
                    goto out;
            }
            av_opt_freep_ranges(&caps_ranges);
        }
    }
    samprates = fmts;
    fmts = NULL;

    if (priv->format != AV_SAMPLE_FMT_NONE) {
        ret = ff_add_format(&fmts, priv->format);
        if (ret < 0)
            goto out;
    } else {
        ret = alsa_query_caps(&caps_ranges, priv->devname, "sample_fmts", false);
        if (ret >= 0) {
            for (n = 0; n < caps_ranges->nb_ranges; n++) {
                ret = ff_add_format(&fmts, caps_ranges->range[n]->value_min);
                if (ret < 0)
                    goto out;
            }
            av_opt_freep_ranges(&caps_ranges);
        }
    }

    if (priv->ch_layout.nb_channels > 0) {
        ret = ff_add_channel_layout(&layouts, &priv->ch_layout);
        if (ret < 0)
            goto out;
    } else {
        ret = alsa_query_caps(&caps_ranges, priv->devname, "channels", false);
        if (ret >= 0) {
            for (n = 0; n < caps_ranges->nb_ranges; n++) {
                int min_ch = caps_ranges->range[n]->value_min;
                int max_ch = caps_ranges->range[n]->is_range ?
                             caps_ranges->range[n]->value_max : min_ch;

                for (j = min_ch; j <= max_ch; j++) {
                    av_channel_layout_default(&layout, j);
                    ret = ff_add_channel_layout(&layouts, &layout);
                    if (ret < 0)
                        goto out;
                }
            }
            av_opt_freep_ranges(&caps_ranges);
        }
    }

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(fmts, &cfg_out[i]->formats);
        if (ret < 0)
            goto out;

        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(samprates, &cfg_out[i]->samplerates);
        if (ret < 0)
            goto out;

        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_out[i]->channel_layouts);
        if (ret < 0)
            goto out;
    }

out:
    av_opt_freep_ranges(&caps_ranges);
    ff_channel_layouts_unref(&layouts);
    ff_formats_unref(&samprates);
    ff_formats_unref(&fmts);

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

static int alsasrc_set_output_volume(AVFilterContext *ctx, int index, double volume)
{
    AlsasrcPriv *priv = ctx->priv;
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

static int alsasrc_get_parameter(AVFilterContext *ctx, const char *key, char *value, int len)
{
    AlsasrcPriv *priv = ctx->priv;

    if (!strcmp(key, "format")) {
        int channels = priv->ch_layout.nb_channels;
        enum AVSampleFormat format = priv->format;
        int sample_rate = priv->sample_rate;
        int nb_channels = 0;
        int pad, ret = 0;

        if (format == AV_SAMPLE_FMT_NONE) {
            int tmp;
            ret = alsasrc_get_device_support_format(ctx, priv->devname,
                                                    "sample_fmts", -1, &tmp);
            if (ret < 0)
                goto format_end;
            format = tmp;
        }

        if (!sample_rate) {
            ret = alsasrc_get_device_support_format(ctx, priv->devname,
                                                    "sample_rates", -1, &sample_rate);
            if (ret < 0)
                goto format_end;
        }

        if (!channels) {
            ret = alsasrc_get_device_support_format(ctx, priv->devname,
                                                    "channels", -1, &channels);
            if (ret < 0)
                goto format_end;
        }

        snprintf(value, len, "fmt=%d:rate=%d:ch=%d", format, sample_rate, channels);
        av_log(ctx, AV_LOG_INFO, "get_parameter: %s = %s\n", key, value);
        return 0;

format_end:
        av_log(ctx, AV_LOG_ERROR, "get_parameter(%s) failed %d.\n", key, ret);
        return ret;
    } else if (!strcmp(key, "volume")) {
        snprintf(value, len, "vol:%f", priv->vol_ctx->volume);

        av_log(priv, AV_LOG_INFO, "get_parameter: %s = %.2f\n", key, priv->vol_ctx->volume);
        return 0;
    }

    av_log(ctx, AV_LOG_ERROR, "get_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

static int alsasrc_set_parameter(AVFilterContext *ctx, const char *args)
{
    AlsasrcPriv *priv = ctx->priv;
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

        if (!strcmp(key, "volume")) {
            double volume;
            int index;

            ret = volume_parse_index_db(value, &index, &volume);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error when parsing %s volume expression '%s'\n",
                       ctx->name, value);
                goto end;
            }

            alsasrc_set_output_volume(ctx, index, volume);
        } else {
            ret = alsa_set_parameter(priv->devname, args);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Unknown parameter: %s\n", key);
            }
        }

end:
        av_freep(&key);
        av_freep(&value);
    }

    return ret;
}

static int alsasrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int ret, i;

    if (!strcmp(cmd, "link")) {
        alsasrc_force_request(ctx);
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        FilterLinkInternal *li;
        AVFilterLink *link;

        if (handle->h) {
            alsasrc_close(ctx);
            alsasrc_set_eof(ctx);
        }

        for (i = 0; i < priv->nb_outputs; i++) {
            li = ff_link_internal(ctx->outputs[i]);
            li->frame_wanted_out = 0;
        }

        return 0;
    } else if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll = (struct pollfd *)res;
        int ret;

        if (!res || res_len < sizeof(struct pollfd))
            return AVERROR(EINVAL);

        if (!handle->h || handle->poll_available >= handle->periods)
            return 0;

        ret = snd_pcm_poll_descriptors(handle->h, poll, 1);

        if (ret < 0)
            return 0;

        return 1;
    } else if (!strcmp(cmd, "poll_available")) {
        snd_pcm_avail_update(handle->h);
        ff_filter_set_ready(ctx, 100);
        handle->poll_available++;
        return 0;
    } else if (!strcmp(cmd, "map")) {
        FilterLinkInternal *li;
        int *old_map = NULL;
        AVFilterLink *link;

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
            link = ctx->outputs[i];
            if (old_map[i] != priv->map[i]) {
                if (old_map[i] == ROUTE_ON && priv->map[i] == ROUTE_OFF) {
                    av_log(ctx, AV_LOG_INFO, "%s disable output%d\n", ctx->name, i);
                    ff_outlink_set_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
                } else if (old_map[i] == ROUTE_OFF && priv->map[i] == ROUTE_ON) {
                    av_log(ctx, AV_LOG_INFO, "%s enable output%d\n", ctx->name, i);
                    li = ff_link_internal(ctx->outputs[i]);
                    li->frame_wanted_out = 1;
                }
            }
        }

        ff_filter_set_ready(ctx, 100);
        av_freep(&old_map);
        return ret;
    } else if (!strcmp(cmd, "get_parameter")) {
        if (!args || res_len <= 0)
            return AVERROR(EINVAL);

        return alsasrc_get_parameter(ctx, args, res, res_len);
    } else if (!strcmp(cmd, "set_parameter")) {
        if (!args)
            return AVERROR(EINVAL);

        return alsasrc_set_parameter(ctx, args);
    } else if (!strcmp(cmd, "dump")) {

        return 0;
    } else if (!strcmp(cmd, "mute") ) {
        priv->mute = true;
        av_log(ctx, AV_LOG_INFO, "set %s mute.", ctx->name);
        return 0;
    } else if (!strcmp(cmd, "unmute") ) {
        priv->mute = false;
        av_log(ctx, AV_LOG_INFO, "set %s unmute.", ctx->name);
        return 0;
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int alsasrc_read_frame(AlsasrcPriv *priv, AVFrame **frame)
{
    AlsaHandle *handle = &priv->priv;
    AVFrame *src;
    int ret;

    src = av_frame_alloc();
    if (!src) {
        av_log(priv, AV_LOG_ERROR, "Failed to allocate frame.\n");
        return AVERROR(ENOMEM);
    }

    src->format = handle->format;
    src->sample_rate = handle->sample_rate;
    av_channel_layout_copy(&src->ch_layout, &handle->ch_layout);
    src->nb_samples = priv->period_size;

    ret = av_frame_get_buffer(src, 0);
    if (ret < 0) {
        av_log(priv, AV_LOG_ERROR, "Failed to allocate frame buffer, ret %d.\n", ret);
        goto fail;
    }

    handle->poll_available = 0;

    ret = alsa_read(handle, src->data[0], priv->period_size);
    if (ret < 0)
        goto fail;

    src->pkt_size = ret * handle->frame_size;
    src->nb_samples = ret;
    src->linesize[0] = src->pkt_size;
    src->time_base =  (AVRational){1, 1000000};
    src->pts = av_rescale_q(priv->timestamp,
                            (AVRational){1, handle->sample_rate},
                            (AVRational){1, 1000000});

    priv->timestamp += ret;

    if (priv->mute)
        memset(src->data[0], 0x00, src->pkt_size);

    *frame = src;
    return ret;

fail:
    av_frame_free(&src);
    return ret;
}

static int alsasrc_check_link_format(const AVFilterLink* link)
{
    if (!link)
        return AVERROR(EINVAL);

    if (link->format <= AV_SAMPLE_FMT_NONE
        || link->format >= AV_SAMPLE_FMT_NB
        || link->sample_rate <= 0 || link->ch_layout.nb_channels <= 0)
        return FFERROR_NOT_READY;

    return 0;
}

static int alsasrc_open(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    AVChannelLayout ch_layout = {0};
    enum AVSampleFormat format;
    AVFilterLink *link;
    int sample_rate;
    int channels;
    int pad = -1;
    int ret;
    int i;

    if (handle->h)
        return 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        link = ctx->outputs[i];
        ret = alsasrc_check_link_format(link);
        if (ret < 0) {
            av_log(ctx, AV_LOG_INFO,
                "%s output%d format not negotiated yet (fmt=%d rate=%d ch=%d), defer open.\n",
                ctx->name, i, link->format, link->sample_rate, link->ch_layout.nb_channels);
            continue;
        }

        pad = i;
        break;
    }

    if (pad == -1)
        return FFERROR_NOT_READY;

    link = ctx->outputs[pad];
    {
        int tmp;
        ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_fmts",
                                                link->format, &tmp);
        if (ret < 0)
            return ret;
        format = tmp;
    }

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_rates",
                                            link->sample_rate, &sample_rate);
    if (ret < 0)
        return ret;

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "channels",
                                            link->ch_layout.nb_channels, &channels);
    if (ret < 0)
        return ret;

    av_channel_layout_default(&ch_layout, channels);

    ret = alsa_open(handle, priv->devname, SND_PCM_STREAM_CAPTURE, sample_rate, ch_layout,
                    format, priv->periods, priv->period_time);
    if (ret < 0)
        return ret;

    priv->period_size = handle->period_time * handle->sample_rate / 1000;
    priv->timestamp = 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        ret = volume_init(&priv->vol_ctx[i], format, priv->precision);
        if (ret < 0)
            goto error;

        if (priv->volume[i] != -1.0f)
            priv->vol_ctx[i].volume = priv->volume[i];
    }

    return 0;

error:
    alsasrc_close(ctx);
    return ret;
}

static int alsasrc_activate(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    AVFrame *frame = NULL;
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

    ret = alsasrc_read_frame(priv, &frame);
    if (ret < 0)
        goto out;

    for (i = 0; i < ctx->nb_outputs; i++) {
        AVFrame *iframe = NULL;

        if ((priv->map && priv->map[i] == ROUTE_OFF) || ff_outlink_get_status(ctx->outputs[i]))
            continue;

        if (!ff_outlink_frame_wanted(ctx->outputs[i]))
            continue;

        iframe = av_frame_clone(frame);
        if (!iframe) {
            ret = AVERROR(ENOMEM);
            goto out;
        }

        volume_scale(&priv->vol_ctx[i], iframe);

        ret = ff_filter_frame(ctx->outputs[i], iframe);
        if (ret < 0)
            goto out;
    }

out:
    av_frame_free(&frame);
    if (ret == AVERROR(EAGAIN))
        return 0;
    return ret;
}

#define OFFSET(x) offsetof(AlsasrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption alsasrc_options[] = {
    { "devname",           "", OFFSET(devname),     AV_OPT_TYPE_STRING,                                     .flags = A },
    { "periods",           "", OFFSET(periods),     AV_OPT_TYPE_INT,        {.i64 = 4},                  0, INT_MAX, R },
    { "period_time",       "", OFFSET(period_time), AV_OPT_TYPE_INT,        {.i64 = 20},                 0, INT_MAX, R },
    { "outputs",           "", OFFSET(nb_outputs),  AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "map",               "", OFFSET(map_str),     AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "map_array",         "", OFFSET(map),         AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { "format",            "", OFFSET(format),      AV_OPT_TYPE_SAMPLE_FMT, {.i64 =AV_SAMPLE_FMT_NONE}, -1, INT_MAX, R },
    { "sample_rate",       "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                  0, INT_MAX, R },
    { "ch_layout",         "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},               0, 0,       R },
    { "precision", "select mathematical precision",
            OFFSET(precision), AV_OPT_TYPE_INT, { .i64 = PRECISION_FIXED }, PRECISION_FIXED, PRECISION_DOUBLE, A, "precision" },
        { "fixed",  "select 8-bit fixed-point",     0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_FIXED  }, INT_MIN, INT_MAX, A, "precision" },
        { "float",  "select 32-bit floating-point", 0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_FLOAT  }, INT_MIN, INT_MAX, A, "precision" },
        { "double", "select 64-bit floating-point", 0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_DOUBLE }, INT_MIN, INT_MAX, A, "precision" },
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
