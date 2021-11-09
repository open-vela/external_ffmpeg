/*
 * FLUORIDE input and output
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
 * FLUORIDE input and output: input
 *
 * This avdevice decoder can capture audio from an FLUORIDE device.
 *
 * The capture period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time capture.
 */

#include "libavformat/internal.h"
#include "libavutil/time.h"
#include "fluoride.h"

#include <poll.h>

static int fluoride_read_close(AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;

    if (priv->st) {
        ff_free_stream(ctx, priv->st);
        priv->st = NULL;
    }

    ff_fluoride_close(ctx);
    return 0;
}

static av_cold int fluoride_read_header(AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;
    int ret;

    priv->st = avformat_new_stream(ctx, NULL);
    if (!priv->st)
        return AVERROR(ENOMEM);

    if (ctx->flags & AVFMT_FLAG_NONBLOCK)
        priv->nonblock = true;

    ret = ff_fluoride_open(ctx);
    if (ret < 0) {
        fluoride_read_close(ctx);
        av_log(ctx, AV_LOG_ERROR, "%s, ret:%d %s\n", __func__, ret, strerror(errno));
        return ret;
    }

    priv->st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    priv->st->codecpar->codec_id    = priv->codec_id;
    priv->st->codecpar->sample_rate = priv->sample_rate;
    priv->st->codecpar->channels    = priv->channels;
    priv->st->codecpar->frame_size  = priv->frame_size;

    avpriv_set_pts_info(priv->st, 64, 1, 1000000);  /* 64 bits pts in us */

    return 0;
}

static int fluoride_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    FluPriv *priv = ctx->priv_data;
    int size;
    int ret;

    ret = av_new_packet(pkt, priv->period_bytes);
    if (ret < 0)
        return AVERROR(EIO);

    size = priv->period_bytes;
    ret = ff_fluoride_read_buffer(priv, pkt->data, size);
    if (ret < 0) {
        av_packet_unref(pkt);
        if (ret != AVERROR(EAGAIN))
            av_log(ctx, AV_LOG_ERROR, "%s, ret:%d %s\n", __func__, ret, strerror(errno));
        return ret;
    }

    pkt->pts = AV_NOPTS_VALUE;
    pkt->size = ret;

    priv->available = false;
    return 0;
}

static int fluoride_dec_init(struct AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;

    priv->data_fd = FLUORIDE_AUDIO_DISCONNECTED;
    priv->ctrl_fd = FLUORIDE_AUDIO_DISCONNECTED;
    priv->codec_id = AV_NE(AV_CODEC_ID_SBC_PACKED, AV_CODEC_ID_SBC_PACKED);
    priv->channels = 1;
    priv->sample_rate = 44100;
    if (ctx->flags & AVFMT_FLAG_NONBLOCK)
        priv->nonblock = true;

    priv->playback = false;

    return ff_fluoride_init_path(priv);
}

static void fluoride_dec_deinit(struct AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;

    ff_fluoride_deinit_path(priv);
}

static int fluoride_dec_control_message(struct AVFormatContext *ctx, int type,
                                 void *data, size_t data_size)
{
    FluPriv *priv = ctx->priv_data;
    struct pollfd *poll = data;
    int ret = 0;

    switch (type) {
        case AV_APP_TO_DEV_GET_POLLFD:
            {
                if (!data || data_size < sizeof(struct pollfd) * 2)
                    return AVERROR(EINVAL);

                if (priv->ctrl_fd <= 0)
                    return AVERROR(EPERM);

                poll[0].fd      = priv->ctrl_fd;
                poll[0].events  = POLLIN;

                if (priv->data_fd != FLUORIDE_AUDIO_DISCONNECTED && !priv->available) {
                    poll[1].fd      = priv->data_fd;
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
                FLUORIDE_CTRL_CMD cmd = FLUORIDE_CTRL_CMD_NONE;
                int state = 0;
                bool start = priv->app_start;

                if (!data || data_size != sizeof(struct pollfd))
                    return AVERROR(EINVAL);

                if (ff_fluoride_data_arrived(priv, poll->fd, &cmd) < 0)
                    break;

                if (priv->data_fd == poll->fd && priv->data_fd != FLUORIDE_AUDIO_DISCONNECTED)
                    priv->available = true;

                avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_BUFFER_READABLE, NULL, 0);
                break;
            }
        default:
            ret = AVERROR(ENOSYS);
            break;
    }

    return ret;
}

#define OFFSET(x) offsetof(FluPriv, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "period_bytes",   "", OFFSET(period_bytes),   AV_OPT_TYPE_INT, {.i64 = 1024},  1, INT_MAX, FLAGS },
    { "frame_size",     "", OFFSET(frame_size),     AV_OPT_TYPE_INT, {.i64 = 2},     1, INT_MAX, FLAGS },
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT, {.i64 = 48000}, 1, INT_MAX, FLAGS },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT, {.i64 = 2},     1, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass fluoride_demuxer_class = {
    .class_name     = "FLUORIDE indev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
};

AVInputFormat ff_fluoride_demuxer = {
    .name                       = "fluoride",
    .long_name                  = NULL_IF_CONFIG_SMALL("FLUORIDE audio input"),
    .priv_data_size             = sizeof(FluPriv),
    .read_header                = fluoride_read_header,
    .read_packet                = fluoride_read_packet,
    .init                       = fluoride_dec_init,
    .deinit                     = fluoride_dec_deinit,
    .control_message            = fluoride_dec_control_message,
    .read_close                 = fluoride_read_close,
    .flags                      = AVFMT_NOFILE,
    .priv_class                 = &fluoride_demuxer_class,
};
