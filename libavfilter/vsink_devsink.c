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

#include <unistd.h>
#include <poll.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include <libavutil/opt.h>
#include <libavutil/eval.h>
#include <libavutil/time.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>

#include "filters.h"
#include "avfilter.h"
#include "internal.h"

enum SyncMode {
    SYNC_MODE_AUDIO,
    SYNC_MODE_SYSTEM,
    SYNC_MODE_BYPASS,
};

typedef struct DevSinkPriv {
    const AVClass   *class;

    AVFormatContext *fmt_ctx;

    char            *format;
    char            *devname;

    int             pixel_fmt;

    int             timer_fd;
    int             frame_duration;
    int             max_latency;
    enum SyncMode   mode;
    int64_t         ts_base;
    int64_t         lat_base;
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

static void devsink_get_audio_timestamp(AVFilterContext *ctx, int64_t *ts, int64_t *lat)
{
    struct AVFilterGraph *graph = ctx->graph;
    DevSinkPriv *priv = ctx->priv;
    AVFilterContext *sink = NULL;
    AVFilterLink *inlink;
    int i;

    *ts = AV_NOPTS_VALUE;
    *lat = 0;

    for (i = 0; i < graph->sink_links_count; i++) {
        AVFilterContext *tmp = graph->sink_links[i]->dst;
        inlink = tmp->inputs[0];
        if (tmp->filter->name && !strcmp(tmp->filter->name, "adevsink") &&
            !ff_outlink_get_status(inlink)) {
            sink = tmp;
            break;
        }
    }

    if (sink) {
        int64_t *res[] = { ts, lat };
        avfilter_process_command(sink, "get_timestamp", NULL, (char *)res, sizeof(res), 0);
    }
}

static void devsink_get_timestamp(AVFilterContext *ctx, int64_t *ts, int64_t *lat)
{
    DevSinkPriv *priv = ctx->priv;
    switch (priv->mode) {
    case SYNC_MODE_AUDIO:
        devsink_get_audio_timestamp(ctx, ts, lat);
        break;
    case SYNC_MODE_SYSTEM:
        *lat = 0;
        *ts = av_gettime_relative();
        break;
    default:
        *lat = 0;
        *ts = AV_NOPTS_VALUE;
        break;
    }
}

static int devsink_sync_video(AVFilterContext *ctx, int64_t pts, int64_t ts, int64_t lat)
{
    DevSinkPriv *priv = ctx->priv;
    int64_t now, diff;

    if (priv->ts_base == AV_NOPTS_VALUE) {
        if (priv->lat_base == AV_NOPTS_VALUE)
            priv->ts_base = ts;
        else
            priv->ts_base = ts - pts;
        priv->lat_base = lat;
        av_log(ctx, AV_LOG_INFO, "sync pts:%" PRId64 " ts:%" PRId64 " base:%" PRId64 " lat:%" PRId64 "\n", pts, ts, priv->ts_base, lat);
    }

    now   = ts - priv->ts_base;
    diff  = pts - now;
    diff += priv->lat_base;

    av_log(ctx, AV_LOG_TRACE, "sync pts:%" PRId64 " ts:%" PRId64 " now:%" PRId64 " diff:%" PRId64 " lat:%" PRId64 "\n", pts, ts, now, diff, lat);

    if (diff > priv->frame_duration)
        return priv->frame_duration;
    else if (diff >= 0)
        return diff;
    else if (diff >= -priv->max_latency)
        return 0;
    else
        return -1;

    return 0;
}

static int devsink_control_message(struct AVFormatContext *s, int type,
                                    void *data, size_t data_size)
{
    AVFilterContext *ctx = s->opaque;

    if (type == AV_DEV_TO_APP_BUFFER_WRITABLE)
        ff_filter_set_ready(ctx, 100);
    else if (type == AV_DEV_TO_APP_STATE_CHANGED) {
        avfilter_graph_reconfig(ctx->graph, ctx);
        ff_filter_set_ready(ctx, 100);
    }

    return 0;
}

static int devsink_start(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    DevSinkPriv *priv = ctx->priv;
    AVStream *st = priv->fmt_ctx->streams[0];
    const AVPixFmtDescriptor *pixdesc;
    int ret;

    if (priv->frame_duration > 0)
        return 0;

    pixdesc = av_pix_fmt_desc_get(inlink->format);

    st->time_base            = inlink->time_base;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = AV_CODEC_ID_RAWVIDEO;
    st->codecpar->format     = inlink->format;
    st->codecpar->width      = inlink->w;
    st->codecpar->height     = inlink->h;
    st->codecpar->bits_per_coded_sample = av_get_bits_per_pixel(pixdesc);

    ret = avformat_write_header(priv->fmt_ctx, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to vdevsink write header, %s\n", av_err2str(ret));
        return ret;
    }
    priv->ts_base  = AV_NOPTS_VALUE;
    priv->lat_base = AV_NOPTS_VALUE;

    priv->frame_duration = av_rescale(AV_TIME_BASE, inlink->frame_rate.den, inlink->frame_rate.num);

    return 0;
}

static void devsink_stop(AVFilterContext *ctx)
{
    DevSinkPriv *priv = ctx->priv;

    if (priv->fmt_ctx)
        avformat_write_trailer(priv->fmt_ctx);
    devsink_timer_stop(ctx);
    priv->frame_duration = 0;
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

    priv->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
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
    int ret;

    ret = frame ? av_write_uncoded_frame(priv->fmt_ctx, 0, frame) : AVERROR_EOF;

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
    int64_t pts, ts, lat;
    int ret;

    if (ff_inlink_check_available_frame(inlink)) {
        ret = devsink_start(ctx);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
            return ret;
        }

        devsink_get_timestamp(ctx, &ts, &lat);
        if (ts != AV_NOPTS_VALUE) {
            frame = ff_inlink_peek_frame(inlink, 0);
            pts = av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q);

            ret = devsink_sync_video(ctx, pts, ts, lat);
            if (ret > 0) {
                devsink_timer_start(ctx, ret);
                return 0;
            }

            devsink_timer_stop(ctx);
        }

        ff_inlink_consume_frame(inlink, &frame);

        if (ret == 0)
            devsink_send_frame(ctx, frame);
        if (ret < 0)
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
    int ret = 0;

    if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll = (struct pollfd *)res;
        int dev_ret;

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
    } else if (!strcmp(cmd, "play")) {
        if (ff_inlink_check_available_frame(ctx->inputs[0]))
            ff_filter_set_ready(ctx, 100);

        ret = avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_PLAY,
                                    res, res_len);
        if (priv->mode == SYNC_MODE_SYSTEM)
            priv->ts_base = AV_NOPTS_VALUE; //for pause to resume

        return ret;
    } else if (!strcmp(cmd, "pause")) {
        devsink_timer_stop(ctx);

        return 0;
    } else if (!strcmp(cmd, "stop")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_STOP,
                                    res, res_len);
    } else if (!strcmp(cmd, "set_parameter")) {
        return avdevice_app_to_dev_control_message(
                                    priv->fmt_ctx,
                                    AV_APP_TO_DEV_SET_PARAMETER,
                                    (char *)args, 0);
    } else if (!strcmp(cmd, "syncmode")) {
        int mode;
        if (args == NULL) {
            av_log(ctx, AV_LOG_ERROR, "Invalid sync mode null\n");
            return AVERROR(EINVAL);
        }

        if (!strcmp(args, "audio"))
            mode = SYNC_MODE_AUDIO;
        else if (!strcmp(args, "system"))
            mode = SYNC_MODE_SYSTEM;
        else if (!strcmp(args, "bypass"))
            mode = SYNC_MODE_BYPASS;
        else {
            av_log(ctx, AV_LOG_ERROR, "Unsupport vsync mode: %s\n", args);
            return AVERROR(EINVAL);
        }

        if (priv->mode != mode) {
            av_log(ctx, AV_LOG_INFO, "Set vsync mode %d,%s from %d\n", mode, args, priv->mode);
            priv->mode = mode;
            priv->ts_base = AV_NOPTS_VALUE;
        } else
            av_log(ctx, AV_LOG_INFO, "vsync mode already set,%d,%s\n", priv->mode, args);

        return 0;
    } else if (!strcmp(cmd, "flush")) {
        devsink_timer_stop(ctx);

        while (ff_inlink_queued_frames(ctx->inputs[0])) {
            AVFrame *frame = NULL;
            ff_inlink_consume_frame(ctx->inputs[0], &frame);
            av_frame_free(&frame);
        }
        ff_filter_set_ready(ctx, 100);
        priv->ts_base = AV_NOPTS_VALUE;

        return avdevice_app_to_dev_control_message(
                                    priv->fmt_ctx,
                                    AV_APP_TO_DEV_FLUSH,
                                    (void *)args, 0);
    } else if (!strcmp(cmd, "dump")) {
        return avdevice_app_to_dev_control_message(
                                    priv->fmt_ctx,
                                    AV_APP_TO_DEV_DUMP,
                                    res, res_len);
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int devsink_forward_command(AVFilterContext *ctx,
                                   int pad_idx, const char *target,
                                   const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    return devsink_process_command(ctx, cmd, args, res, res_len, flags);
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
    { "max_latency", "", OFFSET(max_latency), AV_OPT_TYPE_INT,    {.i64 = 40000},           0,        INT_MAX, FLAGS },
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
    .forward_command = devsink_forward_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};

