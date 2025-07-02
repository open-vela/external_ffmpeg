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
#include "aresample.h"
#include "avfilter_internal.h"
#include "buffersink.h"
#include "buffersrc.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"
#include "volume.h"
#include "subgraph.h"

#define ROUTE_OFF 0
#define ROUTE_ON 1

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
    AResampleContext resample;

    VolumeContext vol_ctx;
    double volume;
    bool mute;

    int *sub_map;
    char *sub_map_str;
    char *sub_desc;
    AVSubGraphContext subgraph;
} AlsasrcPriv;

static inline int alsasrc_subgraph_avaliable(AVFilterContext *ctx, int pad)
{
    AlsasrcPriv *priv = ctx->priv;
    return (priv->sub_map && priv->sub_map[pad]);
}

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

static int alsasrc_config_formats(AVFilterContext *ctx, int pad, AVAudioFormats *dev_fmt,
                                  AVAudioFormats *sub_fmt)
{
    AlsasrcPriv *priv = ctx->priv;
    AVFilterLink *link = ctx->outputs[pad];
    AVAudioFormats suggest_fmt;
    int ret, nb_channels = 0;

    if (!dev_fmt)
        return AVERROR(EINVAL);

    /* if subgragh is enable, use the subgraph input format to negotiate with alsasrc dev,
       or use out link format to negotiate with alsasrc dev */
    if (alsasrc_subgraph_avaliable(ctx, pad)) {
        suggest_fmt.format      = link->format;
        suggest_fmt.sample_rate = link->sample_rate;
        suggest_fmt.ch_layout   = link->ch_layout;

        ret = avfilter_asubgraph_query_formats(priv->sub_desc, &suggest_fmt, dev_fmt, sub_fmt);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Unable to query subgraph formats ret %d\n", ret);
            return ret;
        }
    } else {
        dev_fmt->period_time = 0;
        dev_fmt->format      = link->format;
        dev_fmt->sample_rate = link->sample_rate;
        dev_fmt->ch_layout   = link->ch_layout;
    }

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_fmts",
                                            dev_fmt->format, &dev_fmt->format);
    if (ret < 0)
        goto end;

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_rates",
                                            dev_fmt->sample_rate, &dev_fmt->sample_rate);
    if (ret < 0)
        goto end;

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "channels",
                                            dev_fmt->ch_layout.nb_channels, &nb_channels);
    if (ret < 0)
        goto end;

    if (nb_channels != dev_fmt->ch_layout.nb_channels)
        av_channel_layout_default(&dev_fmt->ch_layout, nb_channels);

end:
    return ret;
}

static void alsasrc_close(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;

    alsa_close(&priv->priv);

    volume_uninit(&priv->vol_ctx);
    avfilter_asubgraph_uninit(&priv->subgraph);
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

    if (priv->sub_map_str) {
        ret = avfilter_parse_mapping(priv->sub_map_str, &priv->sub_map, priv->nb_outputs);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "parse sub_map_str failed ret %d.\n", ret);
            return ret;
        }
    }

    ff_resample_init(&priv->resample);
    priv->format = AV_SAMPLE_FMT_NONE;
    priv->sample_rate = 0;
    priv->volume = -1.0f;
    priv->mute = false;
    av_channel_layout_uninit(&priv->ch_layout);

    return 0;
}

static void alsasrc_uninit(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    int i;

    avfilter_asubgraph_uninit(&priv->subgraph);
    ff_resample_uninit(&priv->resample);

    av_freep(&priv->map);
}

static int alsasrc_query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in,
                                 AVFilterFormatsConfig **cfg_out)
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

static int alsasrc_get_parameter(AVFilterContext *ctx, const char *key, char *value, int len)
{
    AlsasrcPriv *priv = ctx->priv;

    if (!strcmp(key, "format")) {
        enum AVSampleFormat format;
        uint32_t sample_rate;
        int nb_channels;
        int ret = 0;

        if (priv->format != AV_SAMPLE_FMT_NONE) {
            format = priv->format;
        } else {
            ret = alsasrc_get_device_support_format(ctx, priv->devname,
                                                    "sample_fmts", -1, &format);
            if (ret < 0)
                goto format_end;
        }

        if (priv->sample_rate) {
            sample_rate = priv->sample_rate;
        } else {
            ret = alsasrc_get_device_support_format(ctx, priv->devname,
                                                    "sample_rates", -1, &sample_rate);
            if (ret < 0)
                goto format_end;
        }

        if (priv->ch_layout.nb_channels) {
            nb_channels = priv->ch_layout.nb_channels;
        } else {
            ret = alsasrc_get_device_support_format(ctx, priv->devname,
                                                    "channels", -1, &nb_channels);
            if (ret < 0)
                goto format_end;
        }

        snprintf(value, len, "fmt=%d:rate=%d:ch=%d",
                 format, sample_rate, nb_channels);
        av_log(ctx, AV_LOG_INFO, "get_parameter: %s = %s\n", key, value);
        return 0;

format_end:
        av_log(ctx, AV_LOG_ERROR, "get_parameter(%s) failed %d.\n", key, ret);
        return ret;
    } else if (!strcmp(key, "volume")) {
        snprintf(value, len, "vol:%f", priv->vol_ctx.volume);

        av_log(priv, AV_LOG_INFO, "get_parameter: %s = %.2f\n", key, priv->vol_ctx.volume);
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
            av_log(ctx, AV_LOG_ERROR, "No more key-value pairs to parse.\n");
            break;
        }

        if (*p)
            p++;

        av_log(ctx, AV_LOG_INFO, "Parsed Key: %s, Value: %s\n", key, value);

        if (!strcmp(key, "sample_rate")) {
            priv->sample_rate = atoi(value);
            av_log(ctx, AV_LOG_INFO, "Set sample_rate to %d\n", priv->sample_rate);
        } else if (!strcmp(key, "format")) {
            priv->format = av_get_sample_fmt(value);
            av_log(ctx, AV_LOG_INFO, "Set format to %s\n", value);
        } else if (!strcmp(key, "ch_layout")) {
            ret = av_channel_layout_from_string(&priv->ch_layout, value);
            av_log(ctx, AV_LOG_INFO, "Set ch_layout to %s\n", value);
        } else if (!strcmp(key, "volume")) {
            ret = av_expr_parse_and_eval(&priv->volume, value, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error when parsing %s volume expression '%s'\n", ctx->name, value);
                goto end;
            }

            volume_set(&priv->vol_ctx, priv->volume);

            av_log(priv, AV_LOG_INFO, "set_parameter: %s = %.2f\n", key, priv->vol_ctx.volume);
        } else
            av_log(ctx, AV_LOG_ERROR, "Unknown parameter: %s\n", key);

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
    int ret;

    if (!strcmp(cmd, "link")) {
        alsasrc_force_request(ctx);
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        if (handle->h) {
            alsasrc_close(ctx);
            alsasrc_set_eof(ctx);
        }

        av_channel_layout_uninit(&priv->ch_layout);
        priv->format = AV_SAMPLE_FMT_NONE;
        priv->sample_rate = 0;
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
        snd_pcm_avail_update(handle->h);
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
    } else if (!strcmp(cmd, "get_parameter")) {
        if (!args || res_len <= 0)
            return AVERROR(EINVAL);

        return alsasrc_get_parameter(ctx, args, res, res_len);
    }else if (!strcmp(cmd, "sub_map")) {
        ret = avfilter_parse_mapping(args, &priv->sub_map, priv->nb_outputs);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to parse sub_map: %s ret:%d\n", args, ret);
            return ret;
        }

        return 0;
    } else if (!strcmp(cmd, "set_parameter")) {
        if (!args)
            return AVERROR(EINVAL);

        return alsasrc_set_parameter(ctx, args);
    } else if (!strcmp(cmd, "dump") || !strcmp(cmd, "sub_cmd")) {
        return avfilter_asubgraph_process_command(&priv->subgraph, cmd, args, res, res_len, flags);
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

static int alsasrc_open(AVFilterContext *ctx)
{
    AVAudioFormats dev_fmt, sub_fmt;
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int ret;
    int pad;
    int i;

    if (handle->h)
        return 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        pad = i;
        if (priv->sub_map && priv->sub_map[i])
            break;
    }

    ret = alsasrc_config_formats(ctx, pad, &dev_fmt, &sub_fmt);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "config formats failed %d.\n", ret);
        return ret;
    }

    if (alsasrc_subgraph_avaliable(ctx, pad)) {
        ret = avfilter_asubgraph_init(&priv->subgraph, priv->sub_desc, dev_fmt, sub_fmt);
        if (ret < 0) {
           av_log(ctx, AV_LOG_ERROR, "subgraph(%s) init failed %d.\n", priv->sub_desc, ret);
           return ret;
       }
    }
    handle->period_time = dev_fmt.period_time ?
                          dev_fmt.period_time : priv->period_time;
    priv->period_size = handle->period_time * dev_fmt.sample_rate / 1000;
    handle->periods = priv->periods;

    ret = alsa_open(handle, priv->devname, SND_PCM_STREAM_CAPTURE,
                    dev_fmt.sample_rate,
                    dev_fmt.ch_layout.nb_channels,
                    dev_fmt.format);
    if (ret < 0)
        return ret;

    handle->format = dev_fmt.format;
    handle->sample_rate = dev_fmt.sample_rate;
    av_channel_layout_copy(&handle->ch_layout, &dev_fmt.ch_layout);

    priv->timestamp = 0;

    ret = volume_init(&priv->vol_ctx, dev_fmt.format);
    if (ret < 0)
        goto error;

    if (priv->volume != -1.0f)
        priv->vol_ctx.volume = priv->volume;
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
        AVFrame *oframe = NULL;
        AVFrame *iframe = NULL;
        AVFilterLink *link;

        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        iframe = av_frame_clone(frame);
        if (!iframe) {
            av_frame_free(&frame);
            ret = AVERROR(ENOMEM);
            goto out;
        }

        volume_scale(&priv->vol_ctx, iframe);

        if (alsasrc_subgraph_avaliable(ctx, i)) {
            ret = avfilter_asubgraph_process(&priv->subgraph, iframe);
            if (ret < 0)
                continue;
        }

        link = ctx->outputs[i];
        ret = ff_resample_frame(&priv->resample, link, iframe, &oframe);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "resample frame failed %d.\n", ret);
            av_frame_free(&iframe);
            continue;
        }

        av_frame_free(&iframe);

        ret = ff_filter_frame(link, oframe);
        if (ret < 0)
            goto out;
    }

    av_frame_free(&frame);
out:

    if (ret == AVERROR(EAGAIN))
        return 0;
    return ret;
}

#define OFFSET(x) offsetof(AlsasrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption alsasrc_options[] = {
    { "devname",           "", OFFSET(devname),           AV_OPT_TYPE_STRING,     .flags = A },
    { "periods",           "", OFFSET(periods),           AV_OPT_TYPE_INT,        {.i64 = 4},                  0, INT_MAX, R },
    { "period_time",       "", OFFSET(period_time),       AV_OPT_TYPE_INT,        {.i64 = 20},                 0, INT_MAX, R },
    { "outputs",           "", OFFSET(nb_outputs),        AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "map",               "", OFFSET(map_str),           AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "map_array",         "", OFFSET(map),               AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { "sub_desc",          "", OFFSET(sub_desc),          AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "sub_map",           "", OFFSET(sub_map_str),       AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
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
