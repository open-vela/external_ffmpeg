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
 * audio device source
 */

#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavdevice/avdevice.h>
#include <libavformat/internal.h>
#include <libavcodec/avcodec.h>

#include "avfilter.h"
#include "filters.h"
#include "internal.h"

typedef struct ADevSrcPriv {
    const AVClass   *class;

    AVFormatContext *fmt_ctx;
    AVCodecContext  *dec_ctx;

    char            *format;
    char            *devname;

    int             sample_fmt;
    uint32_t        sample_rate;
    uint32_t        channels;
    uint64_t        channel_layout;
} ADevSrcPriv;

static void adevsrc_stop(AVFilterContext *ctx)
{
    ADevSrcPriv *priv = ctx->priv;
    int ret;

    if (!priv->dec_ctx)
        return;

    av_demuxer_close(priv->fmt_ctx);
    avcodec_free_context(&priv->dec_ctx);
}

static int adevsrc_start(AVFilterContext *ctx)
{
    ADevSrcPriv *priv = ctx->priv;
    AVStream *st;
    AVCodec *dec;
    int ret;

    if (priv->dec_ctx)
        return 0;

    ret = av_demuxer_open(priv->fmt_ctx);
    if (ret < 0)
        return ret;

    st = priv->fmt_ctx->streams[0];
    if (!st)
        goto out;

    st->time_base = (AVRational){ 1, st->codecpar->sample_rate };
    st->cur_dts = AV_NOPTS_VALUE;

    /* Find decoder for the stream */
    dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec)
        goto out;

    /* Allocate a codec context for the decoder */
    priv->dec_ctx = avcodec_alloc_context3(dec);
    if (!priv->dec_ctx) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    /* Copy codec parameters from input stream to output codec context */
    ret = avcodec_parameters_to_context(priv->dec_ctx, st->codecpar);
    if (ret < 0)
        goto out;

    /* Init the decoders */
    ret = avcodec_open2(priv->dec_ctx, dec, NULL);
    if (ret < 0)
        goto out;

    return avfilter_graph_reconfig(ctx->graph, NULL);
out:
    adevsrc_stop(ctx);
    return ret;
}

static inline void avdevsrc_force_request(AVFilterContext *ctx)
{
    ctx->outputs[0]->frame_wanted_out = 1;
    ff_filter_set_ready(ctx, 300);
}

static int adevsrc_control_message(struct AVFormatContext *s, int type,
                                    void *data, size_t data_size)
{
    AVFilterContext *ctx = av_format_get_opaque(s);
    ADevSrcPriv *priv = ctx->priv;

    if (type == AV_DEV_TO_APP_STATE_CHANGED)
        avfilter_graph_reconfig(ctx->graph, NULL);

    if (type == AV_DEV_TO_APP_STATE_CHANGED ||
        type == AV_DEV_TO_APP_BUFFER_READABLE) {
        if (priv->dec_ctx)
            ff_filter_set_ready(ctx, 300);
        else
            avdevsrc_force_request(ctx);
    }

    return 0;
}

static int adevsrc_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    ADevSrcPriv *priv = ctx->priv;
    AVInputFormat *fmt = NULL;
    int ret;

    fmt = av_find_input_format(priv->format);
    if (!fmt)
        return AVERROR(EINVAL);

    priv->fmt_ctx = avformat_alloc_context();
    if (!priv->fmt_ctx)
        return AVERROR(ENOMEM);

    av_format_set_opaque(priv->fmt_ctx, ctx);
    av_format_set_control_message_cb(priv->fmt_ctx, adevsrc_control_message);

    priv->fmt_ctx->flags |= AVFMT_FLAG_NONBLOCK | AVFMT_FLAG_PRIV_OPT;
    ret = avformat_open_input(&priv->fmt_ctx, priv->devname, fmt, options);
    if (ret < 0) {
        avformat_free_context(priv->fmt_ctx);
        return ret;
    }

    return 0;
}

static void adevsrc_uninit(AVFilterContext *ctx)
{
    ADevSrcPriv *priv = ctx->priv;

    adevsrc_stop(ctx);
    avformat_close_input(&priv->fmt_ctx);
}

static int adevsrc_activate(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->outputs[0];
    ADevSrcPriv *priv = ctx->priv;
    AVFrame *frame = NULL;
    int ret;

    ret = ff_outlink_get_status(link);
    if (ret < 0) {
        if (ret == AVERROR_EOF)
            adevsrc_stop(ctx);
        return ret;
    }

    if (!ff_outlink_frame_wanted(link))
        return FFERROR_NOT_READY;

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    ret = adevsrc_start(ctx);
    if (ret < 0)
        goto out;

    while (1) {
        AVPacket pkt1, *pkt = &pkt1;

        ret = avcodec_receive_frame(priv->dec_ctx, frame);
        if (ret >= 0)
            break;
        else if (ret != AVERROR(EAGAIN))
            goto out;

        ret = ff_read_packet(priv->fmt_ctx, pkt);
        if (ret < 0)
            goto out;

        ret = avcodec_send_packet(priv->dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto out;
    }

    frame->channel_layout = link->channel_layout;

    return ff_filter_frame(link, frame);

out:
    av_frame_free(&frame);

    if (ret == AVERROR_EOF) {
        adevsrc_stop(ctx);
        ff_avfilter_link_set_in_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
    } else if (ret < 0 && ret != AVERROR(EAGAIN))
        ff_filter_set_ready(ctx, 300);

    return ret;
}

static int adevsrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    ADevSrcPriv *priv = ctx->priv;

    if (!strcmp(cmd, "play")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_PLAY,
                res, res_len);
    } else if (!strcmp(cmd, "pause")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_PAUSE,
                res, res_len);
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
    } else if (!strcmp(cmd, "set_parameter")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_SET_PARAMETER,
                (char *)args, 0);
    } else if (!strcmp(cmd, "dump")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_DUMP,
                res, res_len);
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int adevsrc_query_formats(AVFilterContext *ctx)
{
    AVDeviceCapabilitiesQuery *caps = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    ADevSrcPriv *priv = ctx->priv;
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
                    AVCodec *codec = avcodec_find_decoder(fmt);
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
                    for (i = ranges->range[n]->value_min; i <= ranges->range[n]->value_max; i++) {
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

static int adevsrc_config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    ADevSrcPriv *priv = ctx->priv;
    AVDictionary *fmt_opt = NULL;

    if (!link->channel_layout)
        link->channel_layout = av_get_default_channel_layout(link->channels);

    av_dict_set_int(&fmt_opt, "sample_rate", link->sample_rate, 0);
    av_dict_set_int(&fmt_opt, "channels", link->channels, 0);
    av_dict_set_int(&fmt_opt, "channel_layout", link->channel_layout, 0);

    av_opt_set_dict(priv->fmt_ctx->priv_data, &fmt_opt);
    av_dict_free(&fmt_opt);

    return 0;
}

static void *adevsrc_child_next(void *obj, void *prev)
{
    ADevSrcPriv *priv = obj;

    if (!prev)
        return priv->fmt_ctx;
    else if (prev == priv->fmt_ctx)
        return priv->dec_ctx;
    else
        return NULL;
}

static const struct AVClass *adevsrc_child_class_next(const struct AVClass *prev)
{
    if (!prev)
        return avformat_get_class();
    else if (prev == avformat_get_class())
        return avcodec_get_class();
    else
        return NULL;
}

#define OFFSET(x) offsetof(ADevSrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption adevsrc_options[] = {
    { "format",         "", OFFSET(format),         AV_OPT_TYPE_STRING,         .flags = A },
    { "devname",        "", OFFSET(devname),        AV_OPT_TYPE_STRING,         .flags = A },
    { "sample_fmt",     "", OFFSET(sample_fmt),     AV_OPT_TYPE_SAMPLE_FMT,     {.i64=AV_SAMPLE_FMT_NONE}, -1, INT_MAX, R },
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT,            {.i64 = 0},                 0, INT_MAX, R },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT,            {.i64 = 0},                 0, INT_MAX, R },
    { "channel_layout", "", OFFSET(channel_layout), AV_OPT_TYPE_CHANNEL_LAYOUT, {.i64 = 0},                 0, INT_MAX, R },
    { NULL },
};

static const AVClass adevsrc_class = {
    .class_name = "adevsrc_class",
    .item_name  = av_default_item_name,
    .option     = adevsrc_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
    .child_next = adevsrc_child_next,
    .child_class_next = adevsrc_child_class_next,
};

static const AVFilterPad adevsrc_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = adevsrc_config_props,
    },
    { NULL }
};

AVFilter ff_asrc_adevsrc = {
    .name            = "adevsrc",
    .description     = NULL_IF_CONFIG_SMALL("audio device source"),
    .priv_size       = sizeof(ADevSrcPriv),
    .priv_class      = &adevsrc_class,
    .query_formats   = adevsrc_query_formats,
    .init_dict       = adevsrc_init_dict,
    .uninit          = adevsrc_uninit,
    .outputs         = adevsrc_outputs,
    .activate        = adevsrc_activate,
    .process_command = adevsrc_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
