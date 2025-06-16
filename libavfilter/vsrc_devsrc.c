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
 * video device source
 */

#include <unistd.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavdevice/avdevice.h>
#include <libavformat/internal.h>
#include <libavformat/demux.h>
#include <libavcodec/avcodec.h>

#include "avfilter.h"
#include "filters.h"
#include "internal.h"

typedef struct DevSrcPriv {
    const AVClass   *class;

    AVFormatContext *fmt_ctx;
    AVCodecContext  *dec_ctx;

    char            *format;
    char            *devname;
    int             w, h;
    AVRational      frame_rate;
} DevSrcPriv;

static int devsrc_get_frame_size(AVFilterContext *ctx)
{
    AVDeviceCapabilitiesQuery caps;
    AVOptionRanges *ranges = NULL;
    DevSrcPriv *priv = ctx->priv;
    int ret;

    ret = avdevice_app_to_dev_control_message(priv->fmt_ctx, AV_APP_TO_DEV_GET_CAPS_REQUEST,
                                              &caps, sizeof(caps));
    if (ret < 0)
        return ret == AVERROR(ENOSYS) ? 0 : ret;

    ret = av_opt_query_ranges(&ranges, &caps, "video_size", AV_OPT_MULTI_COMPONENT_RANGE);
    if (ret >= 0) {
        priv->w = (int)ranges->range[0]->value_min;
        priv->h = (int)ranges->range[0]->value_max;
        ret = 0;
    }

    av_opt_freep_ranges(&ranges);
    return ret;
}

static void devsrc_stop(AVFilterContext *ctx)
{
    DevSrcPriv *priv = ctx->priv;

    if (!priv->dec_ctx)
        return;

    avformat_read_close(priv->fmt_ctx);
    avcodec_free_context(&priv->dec_ctx);
}

static int devsrc_start(AVFilterContext *ctx)
{
    DevSrcPriv *priv = ctx->priv;
    const AVCodec *dec;
    AVStream *st;
    int ret;

    if (priv->dec_ctx)
        return 0;

    ret = avformat_read_header(priv->fmt_ctx);
    if (ret < 0)
        return ret;

    st = priv->fmt_ctx->streams[0];
    if (!st)
        goto out;

    /* use a high resolution time base,
     * the maximum timebase for mpeg4 video encoder is 1/65535
     */
    st->time_base = av_make_q(1, 65535);

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

    return avfilter_graph_reconfig(ctx->graph, ctx);
out:
    devsrc_stop(ctx);
    return ret;
}

static inline void devsrc_force_request(AVFilterContext *ctx)
{
    ctx->outputs[0]->frame_wanted_out = 1;
    ff_filter_set_ready(ctx, 300);
}

static int devsrc_control_message(struct AVFormatContext *s, int type,
                                    void *data, size_t data_size)
{
    AVFilterContext *ctx = s->opaque;
    DevSrcPriv *priv = ctx->priv;

    if (type == AV_DEV_TO_APP_STATE_CHANGED)
        avfilter_graph_reconfig(ctx->graph, ctx);

    if (type == AV_DEV_TO_APP_STATE_CHANGED ||
        type == AV_DEV_TO_APP_BUFFER_READABLE) {
        if (priv->dec_ctx)
            ff_filter_set_ready(ctx, 300);
        else
            devsrc_force_request(ctx);
    }

    return 0;
}

static int devsrc_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    DevSrcPriv *priv = ctx->priv;
    const AVInputFormat *fmt = NULL;
    char tmp[16];
    int ret;

    if (access(priv->devname, F_OK) < 0) {
        av_log(priv, AV_LOG_WARNING, " %s path is no exist!\n", priv->devname);
        av_dict_free(options);
        return 0;
    }

    fmt = av_find_input_format(priv->format);
    if (!fmt)
        return AVERROR(EINVAL);

    priv->fmt_ctx = avformat_alloc_context();
    if (!priv->fmt_ctx)
        return AVERROR(ENOMEM);

    priv->fmt_ctx->opaque             = ctx;
    priv->fmt_ctx->control_message_cb = devsrc_control_message;
    priv->fmt_ctx->flags             |= AVFMT_FLAG_NONBLOCK | AVFMT_FLAG_PRIV_OPT;

    if (priv->w && priv->h) {
        snprintf(tmp, sizeof(tmp), "%dx%d", priv->w, priv->h);
        av_dict_set(options, "video_size", tmp, 0);
    }

    if (priv->frame_rate.den && priv->frame_rate.num) {
        snprintf(tmp, sizeof(tmp), "%d/%d", priv->frame_rate.num, priv->frame_rate.den);
        av_dict_set(options, "framerate", tmp, 0);
    }

    ret = avformat_open_input(&priv->fmt_ctx, priv->devname, fmt, options);
    if (ret < 0) {
        avformat_free_context(priv->fmt_ctx);
        priv->fmt_ctx = NULL;
        return ret;
    }

    if (!priv->w || !priv->h) {
        ret = devsrc_get_frame_size(ctx);
    }

    return ret;
}

static void devsrc_uninit(AVFilterContext *ctx)
{
    DevSrcPriv *priv = ctx->priv;

    devsrc_stop(ctx);
    avformat_close_input(&priv->fmt_ctx);
}

static int devsrc_activate(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->outputs[0];
    DevSrcPriv *priv = ctx->priv;
    AVFrame *frame = NULL;
    int ret;

    ret = ff_outlink_get_status(link);
    if (ret < 0) {
        if (ret == AVERROR_EOF)
            devsrc_stop(ctx);
        return ret;
    }

    if (!ff_outlink_frame_wanted(link))
        return FFERROR_NOT_READY;

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    ret = devsrc_start(ctx);
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

    return ff_filter_frame(link, frame);

out:
    av_frame_free(&frame);

    if (ret == AVERROR_EOF) {
        devsrc_stop(ctx);
        ff_avfilter_link_set_in_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
    } else if (ret < 0 && ret != AVERROR(EAGAIN))
        ff_filter_set_ready(ctx, 300);

    return ret;
}

static int devsrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    DevSrcPriv *priv = ctx->priv;

    if (!priv->fmt_ctx)
        return 0;

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
    } else if (!strcmp(cmd, "dump")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_DUMP,
                res, res_len);
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int devsrc_query_formats(AVFilterContext *ctx)
{
    AVDeviceCapabilitiesQuery caps;
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    AVOptionRanges *ranges = NULL;
    DevSrcPriv *priv = ctx->priv;
    enum AVPixelFormat format;
    int ret, i, j;

    if (!priv->fmt_ctx)
        return FFERROR_NOT_READY;

    ret = avdevice_app_to_dev_control_message(priv->fmt_ctx, AV_APP_TO_DEV_GET_CAPS_REQUEST,
                                              &caps, sizeof(caps));
    if (ret < 0)
        return ret == AVERROR(ENOSYS) ? 0 : ret;

    format = av_get_pix_fmt(priv->format);

    if (format != AV_PIX_FMT_NONE) {
        ret = ff_add_format(&formats, format);
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
        } else if (ret == AVERROR(ENODEV) || ret == AVERROR(EINVAL)) {
            /* By default, no camera(/dev/videoX) in system, so system
             * load graph failed after the graph updated with camera function.
             * Add this segment for system can boot normally even if no camera */
            ret = FFERROR_NOT_READY;
            goto out;
        }
    }

    return ff_set_common_formats(ctx, formats);

out:
    av_opt_freep_ranges(&ranges);
    return ret;
}

static int devsrc_config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    DevSrcPriv *priv = ctx->priv;
    AVDictionary *fmt_opt = NULL;

    link->w = priv->w;
    link->h = priv->h;
    link->frame_rate = priv->frame_rate;
    /* use a high resolution time base,
     * the maximum timebase for mpeg4 video encoder is 1/65535
     */
    link->time_base = av_make_q(1, 65535);

    return 0;
}

static void *devsrc_child_next(void *obj, void *prev)
{
    DevSrcPriv *priv = obj;

    if (!prev)
        return priv->fmt_ctx;
    else if (prev == priv->fmt_ctx)
        return priv->dec_ctx;
    else
        return NULL;
}

static const struct AVClass *devsrc_child_class_iterate(void **iter)
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

#define OFFSET(x) offsetof(DevSrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption devsrc_options[] = {
    { "format",      "set video format", OFFSET(format),      AV_OPT_TYPE_STRING,     {.str = "v4l2"}, 0, 0, A },
    { "f",           "set video format", OFFSET(format),      AV_OPT_TYPE_STRING,     {.str = "v4l2"}, 0, 0, A },
    { "devname",     "set device path",  OFFSET(devname),     AV_OPT_TYPE_STRING,     {.str = NULL},   0, 0, A },
    { "d",           "set device path",  OFFSET(devname),     AV_OPT_TYPE_STRING,     {.str = NULL},   0, 0, A },
    { "size",        "set video size",   OFFSET(w),           AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL},   0, 0, A },
    { "s",           "set video size",   OFFSET(w),           AV_OPT_TYPE_IMAGE_SIZE, {.str = NULL},   0, 0, A },
    { "rate",        "set video rate",   OFFSET(frame_rate),  AV_OPT_TYPE_VIDEO_RATE, {.str = "25"},   0, INT_MAX, R },
    { "r",           "set video rate",   OFFSET(frame_rate),  AV_OPT_TYPE_VIDEO_RATE, {.str = "25"},   0, INT_MAX, R },
    { NULL },
};

static const AVClass devsrc_class = {
    .class_name          = "devsrc_class",
    .item_name           = av_default_item_name,
    .option              = devsrc_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = devsrc_child_next,
    .child_class_iterate = devsrc_child_class_iterate,
};

static const AVFilterPad devsrc_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = devsrc_config_props,
    },
};

const AVFilter ff_vsrc_devsrc = {
    .name            = "devsrc",
    .description     = NULL_IF_CONFIG_SMALL("video device source"),
    .priv_size       = sizeof(DevSrcPriv),
    .priv_class      = &devsrc_class,
    .init_dict       = devsrc_init_dict,
    .uninit          = devsrc_uninit,
    FILTER_OUTPUTS(devsrc_outputs),
    FILTER_QUERY_FUNC(devsrc_query_formats),
    .activate        = devsrc_activate,
    .process_command = devsrc_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
