/*
 * Copyright (c) 2023 xiaomi corp
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

#include "avdevice.h"
#include "libavfilter/framequeue.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/thread.h"
#include "libavutil/mem.h"
#include "libavformat/mux.h"
#include "libavformat/network.h"

#include <uikit/video/uikit_vtun.h>

#ifdef CONFIG_NET_RPMSG
#include <netpacket/rpmsg.h>
#endif // CONFIG_NET_RPMSG

#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#define RPMSG_HEADER "rpmsg>"

typedef struct {
    vg_vtun_frame_format tunfmt;
    enum AVPixelFormat pixfmt;
} VtunPixFmt;

typedef struct {
    vg_vtun_frame tunframe;
    AVFrame *avframe;
} VtunShareFrame;

typedef struct {
    AVClass *class; ///< class for private options
    VtunShareFrame frame;
    FFFrameQueue queue;
    char *server_path;
    int frame_count;
    int drop_count;
    int listen_fd;
    int ctrl_fd;
    bool stop;
} VtunCtx;

static const VtunPixFmt ff_vtun_pixfmt_map[] = {
    { VTUN_FRAME_FORMAT_BGRA8888, AV_PIX_FMT_BGRA },
    { VTUN_FRAME_FORMAT_NV12, AV_PIX_FMT_NV12 },
    { VTUN_FRAME_FORMAT_NV21, AV_PIX_FMT_NV21 },
    { VTUN_FRAME_FORMAT_RGB565, AV_PIX_FMT_RGB565LE },
};

static vg_vtun_frame_format vtun_format_convert(enum AVPixelFormat format)
{
    int nb_pixfmts = FF_ARRAY_ELEMS(ff_vtun_pixfmt_map);
    int i;

    for (i = 0; i < nb_pixfmts; i++) {
        if (format == ff_vtun_pixfmt_map[i].pixfmt)
            return ff_vtun_pixfmt_map[i].tunfmt;
    }

    return VTUN_FRAME_FORMAT_INVALID;
}

static int vtun_server_open(VtunCtx *priv)
{
    int flags = SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK;
    const char *url = priv->server_path;
    int fd, ret;

    if (!priv->server_path) {
        av_log(priv, AV_LOG_ERROR, "NO server path found.\n");
        return AVERROR(EINVAL);
    }

#ifdef CONFIG_NET_RPMSG
    if (av_strstart(priv->server_path, RPMSG_HEADER, &url)) {
        struct sockaddr_rpmsg addr;
        fd = socket(AF_RPMSG, flags, 0);
        if (fd < 0)
            return ff_neterrno();

        memset(&addr, 0, sizeof(addr));
        av_strlcpy(addr.rp_name, url, sizeof(addr.rp_name));
        addr.rp_family = AF_RPMSG;

        if ((ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr))) < 0)
            goto fail;
    }
    else
#endif
    {
        struct sockaddr_un addr;
        fd = socket(AF_LOCAL, flags, 0);
        if (fd < 0)
            return ff_neterrno();

        memset(&addr, 0, sizeof(addr));
        av_strlcpy(addr.sun_path, url, sizeof(addr.sun_path));
        addr.sun_family = AF_UNIX;

        if ((ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr))) < 0)
            goto fail;
    }

    if ((ret = listen(fd, 1)) < 0)
        goto fail;

    priv->listen_fd = fd;
    return 0;
fail:
    closesocket(fd);
    return ret;
}

static int vtun_recv_ctrl(VtunCtx *priv, void *buffer, size_t length)
{
    while(length > 0) {
        ssize_t ret = recv(priv->ctrl_fd, buffer, length, MSG_NOSIGNAL);
        if (ret < 0)
            return AVERROR(errno);

        buffer = (char*)buffer + ret;
        length -= ret;
    }

    return 0;
}

static int vtun_send_ctrl(VtunCtx *priv, const void *buffer, size_t length)
{
    while (length > 0) {
        ssize_t ret = send(priv->ctrl_fd, buffer, length, MSG_NOSIGNAL);
        if (ret < 0)
            return AVERROR(errno);

        buffer = (const char*)buffer + ret;
        length -= ret;
    }

    return 0;
}

static vg_vtun_frame *vtun_get_frame(VtunCtx *priv)
{
    VtunShareFrame *frame = &priv->frame;
    AVFrame *avframe;
    int i;

    if (ff_framequeue_queued_frames(&priv->queue) > 0) {
        if (frame->avframe)
            av_frame_free(&frame->avframe);

        avframe = ff_framequeue_take(&priv->queue);

        frame->tunframe.format = vtun_format_convert(avframe->format);
        frame->tunframe.current_ms = av_rescale_q(avframe->pts,
                                                  avframe->time_base, av_make_q(1, 1000));
        for (i = 0; i < FFMIN(VTUN_FRAME_PLANE_NUM, AV_NUM_DATA_POINTERS); i++) {
            frame->tunframe.plane[i].addr = avframe->data[i];
            frame->tunframe.plane[i].stride = avframe->linesize[i];
        }

        frame->tunframe.w = avframe->width;
        frame->tunframe.h = avframe->height;
        frame->tunframe.crop_info.y1 = avframe->crop_top;
        frame->tunframe.crop_info.y2 = avframe->crop_bottom;
        frame->tunframe.crop_info.x1 = avframe->crop_left;
        frame->tunframe.crop_info.x2 = avframe->crop_right;
        frame->avframe = avframe;
        return &frame->tunframe;
    }

    return NULL;
}

static int vtun_handle_event(struct AVFormatContext *h)
{
    vg_vtun_frame *frame;
    uint8_t event;
    int ret;
    VtunCtx *priv = h->priv_data;

    if ((ret = vtun_recv_ctrl(priv, &event, sizeof(event))) < 0)
        return ret;

    switch (event) {
        case VTUN_CTRL_EVT_FRAME_REQ: {
            frame = vtun_get_frame(priv);
            ret = vtun_send_ctrl(priv, &frame, sizeof(frame));
            break;
        }
        case VTUN_CTRL_EVT_PLAY: {
            priv->stop = false;
            ret = avdevice_dev_to_app_control_message(h, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
            break;
        }
        case VTUN_CTRL_EVT_STOP: {
            priv->stop = true;
            ret = avdevice_dev_to_app_control_message(h, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
            break;
        }
        default:
            break;
    }
    return ret;
}

static int vtun_init(AVFormatContext *h)
{
    VtunCtx *priv = h->priv_data;
    int ret;

    if ((ret = vtun_server_open(priv)) < 0)
        return ret;
    ff_framequeue_init(&priv->queue, NULL);

    return 1;
}

static void vtun_deinit(AVFormatContext *h)
{
    VtunCtx *priv = h->priv_data;

    ff_framequeue_free(&priv->queue);
    closesocket(priv->listen_fd);
}

static int vtun_write_header(AVFormatContext *h)
{
    VtunCtx *priv = (VtunCtx *)h->priv_data;

    if (priv->stop)
        return AVERROR(EOF);

    if (h->nb_streams != 1 || h->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        av_log(priv, AV_LOG_ERROR, "Only a single video stream is supported.\n");
        return AVERROR(EINVAL);
    }

    priv->drop_count = 0;

    return 0;
}

static int vtun_write_uncoded_frame(AVFormatContext *h, int stream_index,
                                    AVFrame **frame, unsigned flags)
{
    VtunCtx *priv = (VtunCtx *)h->priv_data;
    int ret = AVERROR(EINVAL);
    AVFrame *dequeue_frame;
    AVFrame *new_frame;

    if (priv->stop)
        return AVERROR(EOF);

    if (flags & AV_WRITE_UNCODED_FRAME_QUERY)
        return 0;

    new_frame = av_frame_clone(*frame);
    if (!new_frame)
        return AVERROR(ENOMEM);

    ret = ff_framequeue_add(&priv->queue, new_frame);
    if (ret < 0) {
        av_log(priv, AV_LOG_WARNING, "%s: frame enqueue failed\n", __func__);
        av_frame_free(&new_frame);
        return ret;
    }

    if (ff_framequeue_queued_frames(&priv->queue) > priv->frame_count) {
        int selected = -1;
        int i, count;
        int64_t min_duration = INT64_MAX;

        count = ff_framequeue_queued_frames(&priv->queue);
        for (i = 1; i < count; i++) {
            AVFrame *f0 = ff_framequeue_peek(&priv->queue, i - 1);
            AVFrame *f1 = ff_framequeue_peek(&priv->queue, i);
            int64_t duration = f1->pts - f0->pts;
            if (duration < min_duration) {
                min_duration = duration;
                selected = i;
            }
        }

        if (selected < 0) {
            av_log(priv, AV_LOG_WARNING, "No frame selected\n");
            return AVERROR(EINVAL);
        }

        dequeue_frame = ff_framequeue_take_index(&priv->queue, selected);
        if (!dequeue_frame) {
            av_log(priv, AV_LOG_WARNING, "No frame dequeued\n");
            return AVERROR(EINVAL);
        }

        av_log(priv, AV_LOG_DEBUG, "vtun drop frame pts:%" PRId64 " selected=%d\n", dequeue_frame->pts, selected);

        av_frame_free(&dequeue_frame);
        priv->drop_count++;
    }

    return ret;
}

static int vtun_write_trailer(AVFormatContext *h)
{
    VtunCtx *priv = h->priv_data;

    if (priv->frame.avframe)
        av_frame_free(&priv->frame.avframe);

    while (ff_framequeue_queued_frames(&priv->queue)) {
        AVFrame *avframe = ff_framequeue_take(&priv->queue);
        av_frame_free(&avframe);
    }

    if (priv->ctrl_fd > 0) {
        closesocket(priv->ctrl_fd);
        priv->ctrl_fd = 0;
    }

    return 0;
}

static int vtun_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                       const char *key, int flags)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    struct AVFormatContext *h = devcap->device_context;
    struct AVOptionRanges *ranges;
    enum AVPixelFormat pix_fmt;
    int ret = AVERROR(ENOMEM);
    int i;

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        goto err;

    if (strcmp(key, "pixel_fmts"))
        goto err;

    ranges->nb_components = 1;
    ranges->nb_ranges = FF_ARRAY_ELEMS(ff_vtun_pixfmt_map);
    ranges->range = av_mallocz(sizeof(AVOptionRange *) * ranges->nb_ranges);

    if (!ranges->range)
        goto err;

    for (i = 0; i < ranges->nb_ranges; i++) {
        ranges->range[i] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[i])
            goto err;

        ranges->range[i]->is_range = 0;
        ranges->range[i]->value_min = ff_vtun_pixfmt_map[i].pixfmt;
        ranges->range[i]->value_max = ff_vtun_pixfmt_map[i].pixfmt;
    }

    *ranges_ = ranges;
    return ranges->nb_components;

err:
    av_opt_freep_ranges(&ranges);
    return ret;
}

static const AVClass vtun_cap_class = {
    .class_name   = "vtun outdev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges = vtun_capbility_query_ranges,
};

static int vtun_control_message(struct AVFormatContext *h, int type,
                                void *data, size_t data_size)
{
    VtunCtx *priv = h->priv_data;
    struct pollfd *poll = data;
    int ret = 0;

    switch (type) {
        case AV_APP_TO_DEV_GET_CAPS_REQUEST: {
            struct AVDeviceCapabilitiesQuery *caps = data;

            if (!caps)
                return AVERROR(EINVAL);

            caps->av_class = &vtun_cap_class;
            caps->device_context = h;
            av_opt_set_defaults(caps);
            break;
        }
        case AV_APP_TO_DEV_GET_POLLFD: {
            if (!data || data_size < sizeof(struct pollfd))
                return AVERROR(EINVAL);

            if (priv->ctrl_fd > 0) {
                poll[ret].fd = priv->ctrl_fd;
                poll[ret].events = POLLIN | POLLHUP;
                ret++;
            } else if (priv->listen_fd > 0) {
                poll[ret].fd = priv->listen_fd;
                poll[ret].events = POLLIN;
                ret++;
            }
            break;
        }
        case AV_APP_TO_DEV_POLL_AVAILABLE: {
            if (!data || data_size != sizeof(struct pollfd))
                return AVERROR(EINVAL);

            if (priv->ctrl_fd == poll->fd) {
                if (poll->revents & POLLHUP) {
                    closesocket(priv->ctrl_fd);
                    priv->ctrl_fd = 0;
                } else {
                    ret = vtun_handle_event(h);
                }
            } else if (priv->listen_fd == poll->fd) {
                priv->ctrl_fd = accept(priv->listen_fd, NULL, NULL);
                if (priv->ctrl_fd < 0)
                    ret = ff_neterrno();
            }
            break;
        }
        case AV_APP_TO_DEV_FLUSH: {
            if (data && !strcmp(data, "eos")) {
                if (priv->frame.avframe)
                    av_frame_free(&priv->frame.avframe);
            }

            while (ff_framequeue_queued_frames(&priv->queue)) {
                AVFrame *avframe = ff_framequeue_take(&priv->queue);
                av_frame_free(&avframe);
            }
            break;
        }

        case AV_APP_TO_DEV_PLAY: {
            priv->stop = false;
            return 0;
        }
        case AV_APP_TO_DEV_DUMP:
            snprintf(data, data_size, "%d|%d", priv->frame_count, priv->drop_count);
            return 0;
        default:
            ret = AVERROR(ENOSYS);
            break;
    }

    return ret;
}

#define OFFSET(x) offsetof(VtunCtx, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "frame_count", "Set frame count", OFFSET(frame_count), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 8, ENC },
    { "server_path", "Set server path", OFFSET(server_path), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, ENC },
    { "stop", "set stop flag init value", OFFSET(stop), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, ENC },
    { NULL }
};

static const AVClass vtun_class = {
    .class_name = "vtun outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

const FFOutputFormat ff_vtun_muxer = {
    .p.name                = "vtun",
    .p.long_name           = NULL_IF_CONFIG_SMALL("video tunnel"),
    .priv_data_size        = sizeof(VtunCtx),
    .p.audio_codec         = AV_CODEC_ID_NONE,
    .p.video_codec         = AV_CODEC_ID_RAWVIDEO,
    .init                  = vtun_init,
    .deinit                = vtun_deinit,
    .write_header          = vtun_write_header,
    .write_uncoded_frame   = vtun_write_uncoded_frame,
    .write_trailer         = vtun_write_trailer,
    .control_message       = vtun_control_message,
    .p.flags               = AVFMT_NOFILE | AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
    .p.priv_class          = &vtun_class
};
