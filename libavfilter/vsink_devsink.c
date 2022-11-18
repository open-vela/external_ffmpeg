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
 * video device sink
 */

#include <libavutil/opt.h>
#include <libavutil/eval.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "filters.h"
#include "avfilter.h"
#include "internal.h"


typedef struct DevSinkPriv {
    const AVClass   *class;

    AVFormatContext *fmt_ctx;
    AVCodecContext  *enc_ctx;

    char            *format;
    char            *devname;

    int             pixel_fmt;

    AVPacket        packet;
} DevSinkPriv;

static int devsink_control_message(struct AVFormatContext *s, int type,
                                    void *data, size_t data_size)
{
    AVFilterContext *ctx = s->opaque;

    if (type == AV_DEV_TO_APP_BUFFER_WRITABLE)
        ff_filter_set_ready(ctx, 100);
    else if (type == AV_DEV_TO_APP_STATE_CHANGED) {
        avfilter_graph_reconfig(ctx->graph, NULL);
        ff_filter_set_ready(ctx, 100);
    }

    return 0;
}

static int devsink_start(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    DevSinkPriv *priv = ctx->priv;
    AVStream *st = priv->fmt_ctx->streams[0];
    AVDictionary *fmt_opt = NULL;
    const AVCodec *enc;
    enum AVCodecID codec_id;
    int ret;

    if (priv->enc_ctx)
        return 0;

    codec_id = priv->fmt_ctx->oformat->video_codec != AV_CODEC_ID_NONE ?
               priv->fmt_ctx->oformat->video_codec : priv->fmt_ctx->video_codec_id;
    enc = avcodec_find_encoder(codec_id);
    if (!enc)
        return AVERROR(EINVAL);

    priv->enc_ctx = avcodec_alloc_context3(enc);
    if (!priv->enc_ctx)
        return AVERROR(ENOMEM);

    priv->enc_ctx->codec_type = inlink->type;
    priv->enc_ctx->pix_fmt    = inlink->format;
    priv->enc_ctx->width      = inlink->w;
    priv->enc_ctx->height     = inlink->h;
    priv->enc_ctx->time_base  = inlink->sample_aspect_ratio;
    av_dict_set_int(&fmt_opt, "w", inlink->w, 0);
    av_dict_set_int(&fmt_opt, "h", inlink->h, 0);
    /* channel_layout  device->avctx->codec */
    avdevice_app_to_dev_control_message(priv->fmt_ctx,
            AV_APP_TO_DEV_GET_FORMAT_REQUEST,
            &fmt_opt, sizeof(AVDictionary *));

    ret = avcodec_open2(priv->enc_ctx, enc, &fmt_opt);
    av_dict_free(&fmt_opt);
    if (ret < 0) {
        avcodec_free_context(&priv->enc_ctx);
        return ret;
    }

    avcodec_parameters_from_context(st->codecpar, priv->enc_ctx);

    ret = avformat_write_header(priv->fmt_ctx, NULL);
    if (ret < 0) {
        avcodec_free_context(&priv->enc_ctx);
        return ret;
    }

    return 0;
}

static void devsink_stop(AVFilterContext *ctx)
{
    DevSinkPriv *priv = ctx->priv;

    if (!priv->enc_ctx)
        return;

    if (priv->packet.data)
        av_packet_unref(&priv->packet);

    avformat_write_trailer(priv->fmt_ctx);
    avcodec_free_context(&priv->enc_ctx);
}

static int devsink_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    DevSinkPriv *priv = ctx->priv;
    AVStream *st;
    int ret;

    ret = avformat_alloc_output_context2(&priv->fmt_ctx, NULL,
                                         priv->format, priv->devname);
    if (ret < 0)
        return ret;

    priv->fmt_ctx->flags             |= AVFMT_FLAG_NONBLOCK;
    priv->fmt_ctx->opaque             = ctx;
    priv->fmt_ctx->control_message_cb = devsink_control_message;

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

static void devsink_uninit(AVFilterContext *ctx)
{
    DevSinkPriv *priv = ctx->priv;

    devsink_stop(ctx);

    avformat_free_context(priv->fmt_ctx);
    priv->fmt_ctx = NULL;
}

static int devsink_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    DevSinkPriv *priv = ctx->priv;
    AVPacket *pkt = &priv->packet;
    int ret;

    if (!priv->enc_ctx)
        return 0;

    ret = avcodec_send_frame(priv->enc_ctx, frame);
    if (ret < 0)
        return ret;

    while (1) {
        ret = avcodec_receive_packet(priv->enc_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                ret = 0;
            break;
        }

        ret = av_write_frame(priv->fmt_ctx, pkt);
        if (ret < 0)
            break;
    }

    if (ret == AVERROR_EOF) {
        ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
        devsink_stop(ctx);
    }

    return ret;
}

static int devsink_activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    DevSinkPriv *priv = ctx->priv;
    AVFrame *frame;
    int64_t pts;
    int ret;

    if (ff_inlink_check_available_frame(inlink)) {
        ret = devsink_start(ctx);
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
            ret = devsink_send_frame(ctx, frame);
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
        return devsink_send_frame(ctx, NULL);

    return ret;
}

static int devsink_query_formats(AVFilterContext *ctx)
{
    AVDeviceCapabilitiesQuery caps;
    AVFilterFormats *formats = NULL;
    DevSinkPriv *priv = ctx->priv;
    AVOptionRanges *ranges = NULL;
    int ret, i, j;

    ret = avdevice_app_to_dev_control_message(priv->fmt_ctx, AV_APP_TO_DEV_GET_CAPS_REQUEST,
                                              &caps, sizeof(caps));
    if (ret < 0)
        return ret == AVERROR(ENOSYS) ? 0 : ret;

    if (priv->pixel_fmt != AV_PIX_FMT_NONE) {
        ret = ff_add_format(&formats, priv->pixel_fmt);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges(&ranges, &caps, "pixel_fmts", AV_OPT_MULTI_COMPONENT_RANGE);
        if (ret >= 0) {
            for (i = 0; i < ranges->nb_ranges; i++) {
                if (ranges->range[i]->is_range) {
                    for (j = ranges->range[i]->value_min; j <= ranges->range[i]->value_max; j++) {
                        ret = ff_add_format(&formats, j);
                        if (ret < 0)
                            goto out;
                    }
                } else {
                    ret = ff_add_format(&formats, ranges->range[i]->value_min);
                    if (ret < 0)
                        goto out;
                }
            }
            av_opt_freep_ranges(&ranges);
        }
    }

    return ff_set_common_formats(ctx, formats);

out:
    av_opt_freep_ranges(&ranges);
    return ret;
}

static int devsink_process_command(AVFilterContext *ctx,
                                    const char *cmd, const char *args,
                                    char *res, int res_len, int flags)
{
    DevSinkPriv *priv = ctx->priv;

    if (!strcmp(cmd, "play")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_PLAY,
                                    res, res_len);
    } else if (!strcmp(cmd, "pause")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
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

static const struct AVClass *devsink_child_class_iterate(void **iter)
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

static void *devsink_child_next(void *obj, void *prev)
{
    DevSinkPriv *priv = obj;

    if (!prev)
        return priv->fmt_ctx;
    else if (prev == priv->fmt_ctx)
        return priv->enc_ctx;
    else
        return NULL;
}

#define OFFSET(x) offsetof(DevSinkPriv, x)
#define FLAGS  AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define FLAGSR FLAGS|AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption devsink_options[] = {
    { "format",    "", OFFSET(format),    AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "devname",   "", OFFSET(devname),   AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "pixel_fmt", "", OFFSET(pixel_fmt), AV_OPT_TYPE_INT,    {.i64 = AV_PIX_FMT_NONE}, -1, INT_MAX, FLAGSR },
    { NULL },
};

static const AVClass devsink_class = {
    .class_name          = "devsink_class",
    .item_name           = av_default_item_name,
    .option              = devsink_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = devsink_child_next,
    .child_class_iterate = devsink_child_class_iterate,
};

static const AVFilterPad devsink_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

const AVFilter ff_vsink_devsink = {
    .name            = "devsink",
    .description     = NULL_IF_CONFIG_SMALL("Video device sink"),
    .priv_class      = &devsink_class,
    .priv_size       = sizeof(DevSinkPriv),
    .init_dict       = devsink_init_dict,
    .uninit          = devsink_uninit,
    .activate        = devsink_activate,
    FILTER_INPUTS(devsink_inputs),
    FILTER_QUERY_FUNC(devsink_query_formats),
    .process_command = devsink_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};

