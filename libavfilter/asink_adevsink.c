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
 * audio device sink
 */

#include <libavutil/opt.h>
#include <libavutil/eval.h>
#include <libavutil/samplefmt.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "filters.h"
#include "avfilter.h"
#include "internal.h"

typedef struct ADevSinkPriv {
    const AVClass   *class;

    AVFormatContext *fmt_ctx;
    AVCodecContext  *enc_ctx;

    char            *format;
    char            *devname;

    int             sample_fmt;
    uint32_t        sample_rate;
    uint32_t        channels;
    uint64_t        channel_layout;

    AVPacket        last_pkt;
} ADevSinkPriv;

static int adevsink_control_message(struct AVFormatContext *s, int type,
                                    void *data, size_t data_size)
{
    AVFilterContext *ctx = av_format_get_opaque(s);
    ADevSinkPriv *priv = ctx->priv;

    if (type == AV_DEV_TO_APP_BUFFER_WRITABLE)
        ff_filter_set_ready(ctx, 100);
    else if (type == AV_DEV_TO_APP_STATE_CHANGED) {
        avfilter_graph_reconfig(ctx->graph, NULL);
        ff_filter_set_ready(ctx, 100);
    }

    return 0;
}

static int adevsink_start(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    ADevSinkPriv *priv = ctx->priv;
    AVStream *st = priv->fmt_ctx->streams[0];
    AVDictionary *fmt_opt = NULL;
    AVCodec *enc;
    int ret;

    if (priv->enc_ctx)
        return 0;

    enc = avcodec_find_encoder(priv->fmt_ctx->oformat->audio_codec);
    if (!enc)
        return AVERROR(EINVAL);

    priv->enc_ctx = avcodec_alloc_context3(enc);
    if (!priv->enc_ctx)
        return AVERROR(ENOMEM);

    priv->enc_ctx->codec_type     = inlink->type;
    priv->enc_ctx->sample_fmt     = inlink->format;
    priv->enc_ctx->sample_rate    = inlink->sample_rate;
    priv->enc_ctx->channel_layout = inlink->channel_layout;
    priv->enc_ctx->channels       = inlink->channels;

    av_dict_set_int(&fmt_opt, "ar", inlink->sample_rate, 0);
    av_dict_set_int(&fmt_opt, "ac", inlink->channels, 0);

    avdevice_app_to_dev_control_message(priv->fmt_ctx,
            AV_APP_TO_DEV_GET_FORMAT_REQUEST,
            &fmt_opt, sizeof(AVDictionary *));

    ret = avcodec_open2(priv->enc_ctx, enc, &fmt_opt);
    av_dict_free(&fmt_opt);
    if (ret < 0) {
        avcodec_free_context(&priv->enc_ctx);
        return ret;
    }

    st->time_base = (AVRational){ 1, inlink->sample_rate };
    st->cur_dts = AV_NOPTS_VALUE;
    avcodec_parameters_from_context(st->codecpar, priv->enc_ctx);

    ret = avformat_write_header(priv->fmt_ctx, NULL);
    if (ret < 0) {
        avcodec_free_context(&priv->enc_ctx);
        return ret;
    }

    return 0;
}

static void adevsink_stop(AVFilterContext *ctx)
{
    ADevSinkPriv *priv = ctx->priv;
    int ret;

    if (!priv->enc_ctx)
        return;

    if (priv->last_pkt.data)
        av_packet_unref(&priv->last_pkt);

    avformat_write_trailer(priv->fmt_ctx);
    avcodec_free_context(&priv->enc_ctx);
}

static int adevsink_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    ADevSinkPriv *priv = ctx->priv;
    AVStream *st;
    int ret;

    ret = avformat_alloc_output_context2(&priv->fmt_ctx, NULL,
                                         priv->format, priv->devname);
    if (ret < 0)
        return ret;

    priv->fmt_ctx->flags |= AVFMT_FLAG_NONBLOCK;
    priv->fmt_ctx->oformat->flags |= AVFMT_NOTIMESTAMPS;

    av_format_set_opaque(priv->fmt_ctx, ctx);
    av_format_set_control_message_cb(priv->fmt_ctx, adevsink_control_message);

    st = avformat_new_stream(priv->fmt_ctx, NULL);
    if (!st) {
        avformat_free_context(priv->fmt_ctx);
        return AVERROR(ENOMEM);
    }

    ret = avformat_init_output(priv->fmt_ctx, options);
    if (ret < 0) {
        avformat_free_context(priv->fmt_ctx);
        return ret;
    }

    return 0;
}

static void adevsink_uninit(AVFilterContext *ctx)
{
    ADevSinkPriv *priv = ctx->priv;

    adevsink_stop(ctx);

    avformat_free_context(priv->fmt_ctx);
    priv->fmt_ctx = NULL;
}

static int adevsink_output_packet(AVFilterContext *ctx)
{
    ADevSinkPriv *priv = ctx->priv;
    AVPacket *pkt = &priv->last_pkt;
    int ret;

    if (!priv->enc_ctx)
        return 0;

    while (1) {
        if (pkt->data) {
            ret = av_write_frame(priv->fmt_ctx, pkt);
            if (ret < 0)
                break;
        }

        ret = avcodec_receive_packet(priv->enc_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                ret = 0;
            break;
        }
    }

    if (ret == AVERROR_EOF) {
        ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
        adevsink_stop(ctx);
    }

    return ret;
}

static int adevsink_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    ADevSinkPriv *priv = ctx->priv;
    int ret;

    if (priv->enc_ctx) {
        ret = avcodec_send_frame(priv->enc_ctx, frame);
        if (ret < 0)
            return ret;
    }

    return adevsink_output_packet(ctx);
}

static int adevsink_activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    ADevSinkPriv *priv = ctx->priv;
    AVFrame *frame;
    int64_t pts;
    int ret;

    ret = adevsink_output_packet(ctx);
    if (ret < 0)
        return ret;

    if (ff_inlink_check_available_frame(inlink)) {
        ret = adevsink_start(ctx);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
            return ret;
        }

        if (priv->enc_ctx->frame_size)
            ret = ff_inlink_consume_samples(inlink, priv->enc_ctx->frame_size, priv->enc_ctx->frame_size, &frame);
        else
            ret = ff_inlink_consume_frame(inlink, &frame);
        if (ret < 0)
            return ret;
        else if (ret > 0) {
            ret = adevsink_send_frame(ctx, frame);
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
        return adevsink_send_frame(ctx, NULL);

    return ret;
}

static int adevsink_query_formats(AVFilterContext *ctx)
{
    AVDeviceCapabilitiesQuery *caps = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    ADevSinkPriv *priv = ctx->priv;
    AVOptionRanges *ranges;
    bool codec = false;
    int ret, i;

    ret = avdevice_capabilities_create(&caps, priv->fmt_ctx, NULL);
    if (ret < 0)
        return ret == AVERROR(ENOSYS) ? 0 : ret;

    if (priv->sample_fmt != AV_SAMPLE_FMT_NONE) {
        ret = ff_add_format(&formats, priv->sample_fmt);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges(&ranges, caps, "sample_fmts", AV_OPT_MULTI_COMPONENT_RANGE);
        if (ret < 0) {
            ret = av_opt_query_ranges(&ranges, caps, "codec", AV_OPT_MULTI_COMPONENT_RANGE);
            codec = true;
        }

        if (ret >= 0) {
            for (i = 0; i < ranges->nb_ranges; i++) {
                int64_t fmt = ranges->range[i]->value_min;

                if (codec) {
                    AVCodec *codec = avcodec_find_encoder(fmt);
                    int n = 0;

                    if (!codec)
                        return AVERROR(EINVAL);

                    while (codec->sample_fmts[n] != AV_SAMPLE_FMT_NONE) {
                        ret = ff_add_format(&formats, codec->sample_fmts[n++]);
                        if (ret < 0)
                            goto out;
                    }
                } else {
                    ret = ff_add_format(&formats, fmt);
                    if (ret < 0)
                        goto out;
                }
            }

            av_opt_freep_ranges(&ranges);
        }
    }

    ret = ff_set_common_formats(ctx, formats);
    if (ret < 0)
        goto out;

    formats = NULL;

    if (priv->sample_rate) {
        ret = ff_add_format(&formats, priv->sample_rate);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges(&ranges, caps, "sample_rates", AV_OPT_MULTI_COMPONENT_RANGE);
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

    if (priv->channels) {
        ret = ff_add_channel_layout(&layouts, FF_COUNT2LAYOUT(priv->channels));
        if (ret < 0)
            goto out;
    } else if (priv->channel_layout) {
        ret = ff_add_channel_layout(&layouts, priv->channel_layout);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges(&ranges, caps, "channels", AV_OPT_MULTI_COMPONENT_RANGE);
        if (ret >= 0) {
            int n;

            for (n = 0; n < ranges->nb_ranges; n++) {
                if (ranges->range[n]->is_range) {
                    for (i = ranges->range[0]->value_min; i <= ranges->range[0]->value_max; i++) {
                        ret = ff_add_channel_layout(&layouts, FF_COUNT2LAYOUT(i));
                        if (ret < 0)
                            goto out;
                    }
                } else {
                    i = ranges->range[n]->value_min;
                    ret = ff_add_channel_layout(&layouts, FF_COUNT2LAYOUT(i));
                    if (ret < 0)
                        goto out;
                }
            }

            av_opt_freep_ranges(&ranges);

        } else {
            ret = av_opt_query_ranges(&ranges, caps, "channel_layout", AV_OPT_MULTI_COMPONENT_RANGE);
            if (ret >= 0) {
                for (i = 0; i < ranges->nb_ranges; i++) {
                    ret = ff_add_channel_layout(&layouts, ranges->range[i]->value_min);
                    if (ret < 0)
                        goto out;
                }

                av_opt_freep_ranges(&ranges);
            }
        }
    }

    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        goto out;

    ret = 0;

out:
    av_opt_freep_ranges(&ranges);
    avdevice_capabilities_free(&caps, priv->fmt_ctx);
    return ret;
}

static int adevsink_process_command(AVFilterContext *ctx,
                                    const char *cmd, const char *args,
                                    char *res, int res_len, int flags)
{
    ADevSinkPriv *priv = ctx->priv;

    if (!strcmp(cmd, "play")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_PLAY,
                                    res, res_len);
    } else if (!strcmp(cmd, "pause")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_PAUSE,
                                    res, res_len);
    } else if (!strcmp(cmd, "volume")) {
        double volume;
        int ret;

        ret = av_expr_parse_and_eval(&volume, args, NULL, NULL,
                                     NULL, NULL, NULL, NULL,
                                     NULL, 0, NULL);
        if (ret < 0)
            return ret;

        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_SET_VOLUME,
                                    &volume, sizeof(double));
    } else if (!strcmp(cmd, "get_pollfd")) {
        return avdevice_app_to_dev_control_message(
                                    priv->fmt_ctx,
                                    AV_APP_TO_DEV_GET_POLLFD,
                                    res, res_len);

    } else if (!strcmp(cmd, "poll_available")) {
        return avdevice_app_to_dev_control_message(
                                    priv->fmt_ctx,
                                    AV_APP_TO_DEV_POLL_AVAILABLE,
                                    res, res_len);
    } else if (!strcmp(cmd, "dump")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_DUMP,
                res, res_len);
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static const struct AVClass* adevsink_child_class_next(const struct AVClass *prev)
{
    if (!prev)
        return avformat_get_class();
    else if (prev == avformat_get_class())
        return avcodec_get_class();
    else
        return NULL;
}

static void* adevsink_child_next(void *obj, void *prev)
{
    ADevSinkPriv *priv = obj;

    if (!prev)
        return priv->fmt_ctx;
    else if (prev == priv->fmt_ctx)
        return priv->enc_ctx;
    else
        return NULL;
}

#define OFFSET(x) offsetof(ADevSinkPriv, x)
#define FLAGS  AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define FLAGSR FLAGS|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption adevsink_options[] = {
    { "format",         "", OFFSET(format),         AV_OPT_TYPE_STRING,         .flags = FLAGS },
    { "devname",        "", OFFSET(devname),        AV_OPT_TYPE_STRING,         .flags = FLAGS },
    { "sample_fmt",     "", OFFSET(sample_fmt),     AV_OPT_TYPE_SAMPLE_FMT,     {.i64=AV_SAMPLE_FMT_NONE}, -1, INT_MAX, FLAGSR },
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT,            {.i64 = 0},                 0, INT_MAX, FLAGSR },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT,            {.i64 = 0},                 0, INT_MAX, FLAGSR },
    { "channel_layout", "", OFFSET(channel_layout), AV_OPT_TYPE_CHANNEL_LAYOUT, {.i64 = 0},                 0, INT_MAX, FLAGSR },
    { NULL },
};

static const AVClass adevsink_class = {
    .class_name       = "adevsink_class",
    .item_name        = av_default_item_name,
    .option           = adevsink_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
    .child_next       = adevsink_child_next,
    .child_class_next = adevsink_child_class_next,
};

static const AVFilterPad adevsink_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_asink_adevsink = {
    .name            = "adevsink",
    .description     = NULL_IF_CONFIG_SMALL("Audio adevice sink"),
    .priv_class      = &adevsink_class,
    .priv_size       = sizeof(ADevSinkPriv),
    .init_dict       = adevsink_init_dict,
    .uninit          = adevsink_uninit,
    .activate        = adevsink_activate,
    .query_formats   = adevsink_query_formats,
    .inputs          = adevsink_inputs,
    .process_command = adevsink_process_command,
};
