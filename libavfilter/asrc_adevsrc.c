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
#include <libavformat/demux.h>
#include <libavcodec/avcodec.h>

#include "avfilter.h"
#include "filters.h"
#include "internal.h"
#include "packet_wrapper.h"

#define ASRC_ADEVSRC_OPENED  1
#define ASRC_ADEVSRC_STARTED 2

typedef struct ADevSrcPriv {
    const AVClass   *class;

    AVFormatContext *fmt_ctx;
    AVCodecContext  *dec_ctx;

    char            *format;
    char            *devname;

    int             sample_fmt;
    uint32_t        sample_rate;
    AVChannelLayout ch_layout;

    int             state;
} ADevSrcPriv;

static void adevsrc_close(AVFilterContext *ctx)
{
    ADevSrcPriv *priv = ctx->priv;

    if (!priv->state)
        return;

    avformat_read_close(priv->fmt_ctx);
    avcodec_free_context(&priv->dec_ctx);
    priv->state = 0;
}

static int adevsrc_open(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->outputs[0];
    ADevSrcPriv *priv = ctx->priv;
    const AVCodec *dec;
    AVStream *st;
    int ret;

    if (priv->state)
        return 0;

    priv->fmt_ctx->audio_codec_id = link->codec;
    ret = avformat_read_header(priv->fmt_ctx);
    if (ret < 0)
        return ret;

    /* offload capture, skip create decoder */
    if (!avcodec_is_pcm_lossless(link->codec))
       goto reconfig;

    st = priv->fmt_ctx->streams[0];
    if (!st)
        goto out;

    st->time_base = (AVRational){ 1, st->codecpar->sample_rate };

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

reconfig:
    priv->state = ASRC_ADEVSRC_OPENED;
    return avfilter_graph_reconfig(ctx->graph, NULL);
out:
    adevsrc_close(ctx);
    return ret;
}

static inline void adevsrc_force_request(AVFilterContext *ctx)
{
    ctx->outputs[0]->frame_wanted_out = 1;
    ff_filter_set_ready(ctx, 300);
}

static int adevsrc_control_message(struct AVFormatContext *s, int type,
                                    void *data, size_t data_size)
{
    AVFilterContext *ctx = s->opaque;
    ADevSrcPriv *priv = ctx->priv;

    if (type == AV_DEV_TO_APP_STATE_CHANGED) {
        avcodec_free_context(&priv->dec_ctx);
        avfilter_graph_reconfig(ctx->graph, NULL);
    }

    if (type == AV_DEV_TO_APP_STATE_CHANGED ||
        type == AV_DEV_TO_APP_BUFFER_READABLE) {
        if (priv->dec_ctx)
            ff_filter_set_ready(ctx, 300);
        else
            adevsrc_force_request(ctx);
    }

    return 0;
}


static int adevsrc_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    ADevSrcPriv *priv = ctx->priv;
    const AVInputFormat *fmt = NULL;
    int ret;

    fmt = av_find_input_format(priv->format);
    if (!fmt)
        return AVERROR(EINVAL);

    priv->fmt_ctx = avformat_alloc_context();
    if (!priv->fmt_ctx)
        return AVERROR(ENOMEM);

    priv->fmt_ctx->opaque             = ctx;
    priv->fmt_ctx->control_message_cb = adevsrc_control_message;
    priv->fmt_ctx->flags             |= AVFMT_FLAG_NONBLOCK | AVFMT_FLAG_PRIV_OPT;

    ret = avformat_open_input(&priv->fmt_ctx, priv->devname, fmt, options);
    if (ret < 0) {
        avformat_free_context(priv->fmt_ctx);
        priv->fmt_ctx = NULL;
        return ret;
    }

    return 0;
}

static void adevsrc_uninit(AVFilterContext *ctx)
{
    ADevSrcPriv *priv = ctx->priv;

    adevsrc_close(ctx);
    avformat_close_input(&priv->fmt_ctx);
}

static int adevsrc_receive_frame(AVFilterContext *ctx, AVFrame **frame)
{
    AVFilterLink *link = ctx->outputs[0];
    ADevSrcPriv *priv = ctx->priv;
    AVFrame *out;
    int ret;

    out = av_frame_alloc();
    if (!out)
        return AVERROR(ENOMEM);

    while (1) {
        AVPacket pkt1, *pkt = &pkt1;

        ret = avcodec_receive_frame(priv->dec_ctx, out);
        if (ret >= 0)
            break;
        else if (ret != AVERROR(EAGAIN))
            goto error;

        ret = ff_read_packet(priv->fmt_ctx, pkt);
        if (ret < 0)
            goto error;

        ret = avcodec_send_packet(priv->dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto error;
    }

    av_channel_layout_copy(&out->ch_layout, &link->ch_layout);
    *frame = out;
    return 0;

error:
    av_frame_free(&out);
    return ret;
}

static int adevsrc_wrap_frame(AVFilterContext *ctx, AVFrame **frame)
{
    AVFilterLink *link = ctx->outputs[0];
    ADevSrcPriv *priv = ctx->priv;
    AVCodecParameters *dst = NULL;
    AVFrame *out = NULL;
    AVPacket *pkt;
    int ret;

    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);

    ret = ff_read_packet(priv->fmt_ctx, pkt);
    if (ret < 0)
        goto error;

    if (!(priv->state & ASRC_ADEVSRC_STARTED)) {
        dst = avcodec_parameters_alloc();
        if (!dst)
            goto error;

        dst->codec_type  = link->type;
        dst->format      = link->format;
        dst->sample_rate = link->sample_rate;
        dst->codec_id    = link->codec;
        av_channel_layout_copy(&dst->ch_layout, &link->ch_layout);

        priv->state = ASRC_ADEVSRC_STARTED;
    }

    out = wrap_frame(pkt, dst, NULL);
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    out->format = link->format;
    out->sample_rate = link->sample_rate;
    av_channel_layout_copy(&out->ch_layout, &link->ch_layout);
    *frame = out;
    return 0;

error:
    av_packet_free(&pkt);
    av_frame_free(&out);
    return ret;
}

static int adevsrc_activate(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->outputs[0];
    ADevSrcPriv *priv = ctx->priv;
    AVFrame *frame;
    int ret;

    ret = ff_outlink_get_status(link);
    if (ret < 0) {
        if (ret == AVERROR_EOF)
            adevsrc_close(ctx);
        return ret;
    }

    if (!ff_outlink_frame_wanted(link))
        return FFERROR_NOT_READY;

    ret = adevsrc_open(ctx);
    if (ret < 0)
        goto out;

    if (priv->dec_ctx)
        ret = adevsrc_receive_frame(ctx, &frame);
    else
        ret = adevsrc_wrap_frame(ctx, &frame);

    if (ret < 0)
        goto out;

    return ff_filter_frame(link, frame);

out:
    if (ret == AVERROR_EOF) {
        adevsrc_close(ctx);
        ff_avfilter_link_set_in_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
    } else if (ret < 0 && ret != AVERROR(EAGAIN))
        ff_filter_set_ready(ctx, 300);

    return ret;
}

static int adevsrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    ADevSrcPriv *priv = ctx->priv;

    if (!strcmp(cmd, "start")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_START,
                res, res_len);
    } else if (!strcmp(cmd, "stop")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_STOP,
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
    } else if (!strcmp(cmd, "mute")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_MUTE,
                NULL, 0);
    } else if (!strcmp(cmd, "unmute")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_UNMUTE,
                NULL, 0);
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
    AVDeviceCapabilitiesQuery caps;
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    ADevSrcPriv *priv = ctx->priv;
    AVOptionRanges *ranges = NULL;
    AVChannelLayout layout;
    bool codec = false;
    int ret, i;

    ret = avdevice_app_to_dev_control_message(priv->fmt_ctx, AV_APP_TO_DEV_GET_CAPS_REQUEST,
                                              &caps, sizeof(caps));
    if (ret < 0)
        return ret == AVERROR(ENOSYS) ? 0 : ret;

    if (priv->sample_fmt != AV_SAMPLE_FMT_NONE) {
        ret = ff_add_format(&formats, priv->sample_fmt);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges(&ranges, &caps, "sample_fmts", AV_OPT_MULTI_COMPONENT_RANGE);
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

    if (priv->sample_rate) {
        ret = ff_add_format(&formats, priv->sample_rate);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges(&ranges, &caps, "sample_rates", AV_OPT_MULTI_COMPONENT_RANGE);
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

    if (priv->ch_layout.nb_channels) {
        ret = ff_add_channel_layout(&layouts, &priv->ch_layout);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges(&ranges, &caps, "channels", AV_OPT_MULTI_COMPONENT_RANGE);
        if (ret >= 0) {
            const AVChannelLayout *layout = NULL;
            double n;

            for (i = 0; i < ranges->nb_ranges; i++) {
                if (ranges->range[i]->is_range) {
                    for (n = ranges->range[i]->value_min; n <= ranges->range[i]->value_max; n++) {
                        void *iter = NULL;
                        while (layout = av_channel_layout_standard(&iter)) {
                            if (layout->nb_channels == n) {
                                ret = ff_add_channel_layout(&layouts, layout);
                                if (ret < 0)
                                    goto out;
                            }
                        }
                    }
                } else {
                    void *iter = NULL;
                    n = ranges->range[i]->value_min;
                    while (layout = av_channel_layout_standard(&iter)) {
                        if (layout->nb_channels == n) {
                            ret = ff_add_channel_layout(&layouts, layout);
                            if (ret < 0)
                                goto out;
                        }
                    }
                }
            }

            av_opt_freep_ranges(&ranges);
        }
    }

    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        goto out;

    formats = NULL;
    ret = av_opt_query_ranges(&ranges, &caps, "codecs", AV_OPT_MULTI_COMPONENT_RANGE);
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

static int adevsrc_config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    ADevSrcPriv *priv = ctx->priv;
    AVDictionary *fmt_opt = NULL;
    char tmp[64];
    int ret;

    av_dict_set_int(&fmt_opt, "sample_rate", link->sample_rate, 0);
    av_channel_layout_describe(&link->ch_layout, tmp, sizeof(tmp));
    av_dict_set(&fmt_opt, "ch_layout", tmp, 0);

    ret = av_opt_set_dict(priv->fmt_ctx->priv_data, &fmt_opt);

    av_dict_free(&fmt_opt);

    return ret;
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

static const struct AVClass *adevsrc_child_class_iterate(void **iter)
{
    const AVClass *c = *iter;

    if (!c)
        c = avformat_get_class();
    else if (c == avformat_get_class())
        c = avcodec_get_class();
    else
        c = NULL;

    *iter = (void*)(uintptr_t)c;
    return *iter;
}

#define OFFSET(x) offsetof(ADevSrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption adevsrc_options[] = {
    { "format",      "", OFFSET(format),      AV_OPT_TYPE_STRING,     .flags = A },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = A },
    { "sample_fmt",  "", OFFSET(sample_fmt),  AV_OPT_TYPE_SAMPLE_FMT, {.i64=AV_SAMPLE_FMT_NONE}, -1, INT_MAX, R },
    { "sample_rate", "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                 0, INT_MAX, R },
    { "ch_layout",   "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},              0, 0,       R },
    { NULL },
};

static const AVClass adevsrc_class = {
    .class_name          = "adevsrc_class",
    .item_name           = av_default_item_name,
    .option              = adevsrc_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = adevsrc_child_next,
    .child_class_iterate = adevsrc_child_class_iterate,
};

static const AVFilterPad adevsrc_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .config_props  = adevsrc_config_props,
    },
};

const AVFilter ff_asrc_adevsrc = {
    .name            = "adevsrc",
    .description     = NULL_IF_CONFIG_SMALL("audio device source"),
    .priv_size       = sizeof(ADevSrcPriv),
    .priv_class      = &adevsrc_class,
    .init_dict       = adevsrc_init_dict,
    .uninit          = adevsrc_uninit,
    FILTER_OUTPUTS(adevsrc_outputs),
    FILTER_QUERY_FUNC(adevsrc_query_formats),
    .activate        = adevsrc_activate,
    .process_command = adevsrc_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
