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

#include <poll.h>
#include <sys/timerfd.h>

#include <libavutil/opt.h>
#include <libavutil/eval.h>
#include <libavutil/time.h>
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

    int             timer_fd;
    AVPacket        packet;
    bool            frame_uncoded;
    int             frame_duration;
    int64_t         last_time;
    int             max_latency;
    int             max_outsync;
    int             ts_offset;
} DevSinkPriv;

static void devsink_timer_start(AVFilterContext *ctx, int us)
{
    DevSinkPriv *priv = ctx->priv;
    struct itimerspec interval;

    interval.it_interval.tv_sec  = 0;
    interval.it_interval.tv_nsec = 1000ll * us;
    interval.it_value            = interval.it_interval;
    timerfd_settime(priv->timer_fd, 0, &interval, NULL);
}

static void devsink_timer_stop(AVFilterContext *ctx)
{
    DevSinkPriv *priv = ctx->priv;
    struct itimerspec interval;

    memset(&interval, 0, sizeof(struct itimerspec));
    timerfd_settime(priv->timer_fd, 0, &interval, NULL);
}

static int64_t devsink_get_audio_timestamp(AVFilterContext *ctx)
{
    struct AVFilterGraph *graph = ctx->graph;
    AVFilterContext *sink = NULL;
    AVFilterLink *inlink;
    int64_t pts = AV_NOPTS_VALUE;
    int i;

    for (i = 0; i < graph->sink_links_count; i++) {
        sink = graph->sink_links[i]->dst;
        inlink = sink->inputs[0];
        if (sink->filter->name && !strcmp(sink->filter->name, "adevsink") &&
            !ff_outlink_get_status(inlink))
            break;
    }

    if (sink)
        avfilter_process_command(sink, "get_timestamp", NULL, (char *)&pts, sizeof(int64_t), 0);

    return pts;
}

static int devsink_sync_video(AVFilterContext *ctx, int64_t apts, int64_t vpts)
{
    DevSinkPriv *priv = ctx->priv;
    int64_t now = av_gettime_relative();
    int64_t diff = 0;

    if (apts >= 0)
        diff = (vpts + priv->ts_offset) - apts;
    else if (priv->last_time)
        diff = priv->frame_duration - (now - priv->last_time);

    if (diff > 0)
        return diff <= priv->max_outsync ? diff : priv->frame_duration;

    priv->last_time = now;
    if (diff >= -priv->max_latency)
        return 0;

    return -1;
}

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
    AVRational *frame_rate = &inlink->frame_rate;
    AVStream *st = priv->fmt_ctx->streams[0];
    AVDictionary *fmt_opt = NULL;
    struct itimerspec interval;
    enum AVCodecID codec_id;
    const AVCodec *enc;
    int ret;

    if (priv->enc_ctx)
        return 0;

    codec_id = priv->fmt_ctx->oformat->video_codec != AV_CODEC_ID_NONE ?
               priv->fmt_ctx->oformat->video_codec : priv->fmt_ctx->video_codec_id;

    priv->frame_uncoded  = codec_id == AV_CODEC_ID_RAWVIDEO && av_write_uncoded_frame_query(priv->fmt_ctx, 0) == 0;
    priv->frame_duration = AV_TIME_BASE * av_q2d(av_inv_q(inlink->frame_rate)); 

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
    priv->enc_ctx->time_base  = av_inv_q(inlink->frame_rate);
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

    priv->last_time = 0;

    return 0;
}

static void devsink_stop(AVFilterContext *ctx)
{
    DevSinkPriv *priv = ctx->priv;
    struct itimerspec interval;

    if (!priv->enc_ctx)
        return;

    if (priv->packet.data)
        av_packet_unref(&priv->packet);

    avformat_write_trailer(priv->fmt_ctx);
    avcodec_free_context(&priv->enc_ctx);

    devsink_timer_stop(ctx);
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
        ret = AVERROR(ENOMEM);
        goto exit;
    }

    if ((ret = avformat_init_output(priv->fmt_ctx, options)) < 0)
        goto exit;

    priv->timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (priv->timer_fd < 0) {
        ret = AVERROR(errno);
        goto exit;
    }

    return 0;

exit:
    avformat_free_context(priv->fmt_ctx);
    priv->fmt_ctx = NULL;

    return ret;
}

static void devsink_uninit(AVFilterContext *ctx)
{
    DevSinkPriv *priv = ctx->priv;

    devsink_stop(ctx);

    avformat_free_context(priv->fmt_ctx);
    priv->fmt_ctx = NULL;
    close(priv->timer_fd);
}

static int devsink_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    DevSinkPriv *priv = ctx->priv;
    AVPacket *pkt = &priv->packet;
    int ret;

    if (priv->frame_uncoded)
        ret = frame ? av_write_uncoded_frame(priv->fmt_ctx, 0, frame) : AVERROR_EOF;
    else {
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

        frame = ff_inlink_peek_frame(inlink, 0);
        ret = devsink_sync_video(ctx, devsink_get_audio_timestamp(ctx),
                                  frame->pts * av_q2d(frame->time_base) * AV_TIME_BASE);
        if (ret > 0) {
            devsink_timer_start(ctx, ret);
            return 0;
        }

        devsink_timer_stop(ctx);
        ff_inlink_consume_frame(inlink, &frame);
        
        if (ret == 0)
            devsink_send_frame(ctx, frame);
        if (ret < 0 || !priv->frame_uncoded)
            av_frame_free(&frame);

        ff_filter_set_ready(ctx, 100);
        return 0;
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

    if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll = (struct pollfd *)res;
        int ret = 0, dev_ret;

        if (!res || res_len < sizeof(struct pollfd))
            return AVERROR(EINVAL);

        poll[ret].fd     = priv->timer_fd;
        poll[ret].events = POLLIN;
        poll++;
        ret++;

        dev_ret = avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_GET_POLLFD,
                poll, res_len - sizeof(struct pollfd));

        if (dev_ret > 0)
            ret += dev_ret;

        return ret;
    } else if (!strcmp(cmd, "poll_available")) {
        struct pollfd *poll = (struct pollfd *)res;

        if (poll->fd == priv->timer_fd) {
            uint64_t tmp;
            if (read(priv->timer_fd, &tmp, sizeof(uint64_t)) < 0)
                return AVERROR(errno);

            ff_filter_set_ready(ctx, 100);
            return 0;
        } else {
            return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_POLL_AVAILABLE,
                res, res_len);
        }
    } else if (!strcmp(cmd, "start")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_START,
                                    res, res_len);
    } else if (!strcmp(cmd, "stop")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_STOP,
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
    { "format",      "", OFFSET(format),      AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING, .flags = FLAGS },
    { "pixel_fmt",   "", OFFSET(pixel_fmt),   AV_OPT_TYPE_INT,    {.i64 = AV_PIX_FMT_NONE}, -1,       INT_MAX, FLAGSR },
    { "max_latency", "", OFFSET(max_latency), AV_OPT_TYPE_INT,    {.i64 = 10000},           0,        INT_MAX, FLAGS },
    { "max_outsync", "", OFFSET(max_outsync), AV_OPT_TYPE_INT,    {.i64 = 200000},          0,        INT_MAX, FLAGS },
    { "ts_offset",   "", OFFSET(ts_offset),   AV_OPT_TYPE_INT,    {.i64 = 0},               -INT_MAX, INT_MAX, FLAGS },
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

