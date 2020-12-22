/*
 * A2DP input and output
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

/**
 * @file
 * A2DP input and output: input
 *
 * This avdevice decoder can capture audio from an A2DP device.
 *
 * The capture period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time capture.
 */

#include "libavformat/internal.h"
#include "libavutil/time.h"
#include "a2dp.h"

#include <poll.h>

static int a2dp_read_close(AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;

    if (a2dp->st) {
        ff_free_stream(ctx, a2dp->st);
        a2dp->st = NULL;
    }

    ff_a2dp_close(ctx);
    return 0;
}

static av_cold int a2dp_read_header(AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;
    int ret;

    a2dp->st = avformat_new_stream(ctx, NULL);
    if (!a2dp->st)
        return AVERROR(ENOMEM);

    if (ctx->flags & AVFMT_FLAG_NONBLOCK)
        a2dp->nonblock = true;

    a2dp->playback = false;

    ret = ff_a2dp_open(ctx);
    if (ret < 0) {
        a2dp_read_close(ctx);
        av_log(ctx, AV_LOG_ERROR, "%s, ret:%d %s\n", __func__, ret, strerror(errno));
        return ret;
    }

    a2dp->st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    a2dp->st->codecpar->codec_id    = a2dp->codec_id;
    a2dp->st->codecpar->sample_rate = a2dp->sample_rate;
    a2dp->st->codecpar->channels    = a2dp->channels;
    a2dp->st->codecpar->frame_size  = a2dp->frame_size;

    avpriv_set_pts_info(a2dp->st, 64, 1, 1000000);  /* 64 bits pts in us */

    return 0;
}

static int a2dp_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    A2dpPriv *a2dp = ctx->priv_data;
    int size;
    int ret;

    ret = av_new_packet(pkt, a2dp->period_bytes);
    if (ret < 0)
        return AVERROR(EIO);

    size = a2dp->period_bytes;
    ret = ff_a2dp_read_buffer(a2dp, pkt->data, size);
    if (ret < 0) {
        av_packet_unref(pkt);
        if (ret != AVERROR(EAGAIN))
            av_log(ctx, AV_LOG_ERROR, "%s, ret:%d %s\n", __func__, ret, strerror(errno));
        return ret;
    }

    pkt->pts = AV_NOPTS_VALUE;
    pkt->size = ret;

    a2dp->available = false;
    return 0;
}

static int a2dp_dec_init(struct AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;

    a2dp->data_fd = A2DP_AUDIO_DISCONNECTED;
    a2dp->ctrl_fd = A2DP_AUDIO_DISCONNECTED;
    a2dp->codec_id = AV_NE(AV_CODEC_ID_SBC_PACKED, AV_CODEC_ID_SBC_PACKED);
    a2dp->channels = 1;
    a2dp->sample_rate = 44100;
    if (ctx->flags & AVFMT_FLAG_NONBLOCK)
        a2dp->nonblock = true;

    return ff_a2dp_init_path(a2dp);
}

static void a2dp_dec_deinit(struct AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;

    ff_a2dp_deinit_path(a2dp);
}

static int a2dp_dec_control_message(struct AVFormatContext *ctx, int type,
                                 void *data, size_t data_size)
{
    A2dpPriv *a2dp = ctx->priv_data;
    struct pollfd *poll = data;
    int ret = 0;

    switch (type) {
        case AV_APP_TO_DEV_GET_POLLFD:
            {
                if (!data || data_size < sizeof(struct pollfd) * 2)
                    return AVERROR(EINVAL);

                if (a2dp->ctrl_fd <= 0)
                    return AVERROR(EPERM);

                poll[0].fd      = a2dp->ctrl_fd;
                poll[0].events  = POLLIN;

                if (a2dp->data_fd != A2DP_AUDIO_DISCONNECTED && !a2dp->available) {
                    poll[1].fd      = a2dp->data_fd;
                    poll[1].events  = POLLIN;
                } else {
                    poll[1].fd      = 0;
                    poll[1].events  = 0;
                }

                poll[2].fd      = 0;
                poll[2].events  = 0;

                break;
            }
        case AV_APP_TO_DEV_POLL_AVAILABLE:
            {
                tA2DP_CTRL_CMD cmd = A2DP_CTRL_CMD_NONE;
                int state = 0;
                bool start = a2dp->app_start;

                if (!data || data_size != sizeof(struct pollfd))
                    return AVERROR(EINVAL);

                if (ff_a2dp_data_arrived(a2dp, poll->fd, &cmd) < 0)
                    break;

                if (a2dp->data_fd == poll->fd && a2dp->data_fd != A2DP_AUDIO_DISCONNECTED)
                    a2dp->available = true;

                avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_BUFFER_READABLE, NULL, 0);
                break;
            }
        default:
            ret = AVERROR(ENOSYS);
            break;
    }

    return ret;
}

#define OFFSET(x) offsetof(A2dpPriv, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "period_bytes",   "", OFFSET(period_bytes),   AV_OPT_TYPE_INT, {.i64 = 1024},  1, INT_MAX, FLAGS },
    { "frame_size",     "", OFFSET(frame_size),     AV_OPT_TYPE_INT, {.i64 = 2},     1, INT_MAX, FLAGS },
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT, {.i64 = 48000}, 1, INT_MAX, FLAGS },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT, {.i64 = 2},     1, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass a2dp_demuxer_class = {
    .class_name     = "A2DP indev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
};

AVInputFormat ff_a2dp_demuxer = {
    .name                       = "a2dp",
    .long_name                  = NULL_IF_CONFIG_SMALL("A2DP audio input"),
    .priv_data_size             = sizeof(A2dpPriv),
    .read_header                = a2dp_read_header,
    .read_packet                = a2dp_read_packet,
    .init                       = a2dp_dec_init,
    .deinit                     = a2dp_dec_deinit,
    .control_message            = a2dp_dec_control_message,
    .read_close                 = a2dp_read_close,
    .flags                      = AVFMT_NOFILE,
    .priv_class                 = &a2dp_demuxer_class,
};
