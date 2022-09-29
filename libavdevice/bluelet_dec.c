/*
 * Bluelet input and output
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
 * Bluelet input and output: input
 *
 * This avdevice decoder can capture audio from an Bluelet device.
 *
 * The capture period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time capture.
 */

#include "libavformat/internal.h"
#include "libavfilter/filters.h"
#include "libavutil/time.h"
#include "bluelet.h"

#include <poll.h>

#define A2DP_PERIOD_BYTES 1024

static const AVClass bluelet_dec_cap_class;

static int bluelet_read_close(AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;

    if (priv->st) {
        ff_free_stream(ctx, priv->st);
        priv->st = NULL;
    }

    return ff_bluelet_stop(priv);
}

static av_cold int bluelet_read_header(AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;
    int ret;

    if (!priv->play)
        return AVERROR_EOF;

    priv->st = avformat_new_stream(ctx, NULL);
    if (!priv->st)
        return AVERROR(ENOMEM);

    ret = ff_bluelet_start(priv);
    if (ret < 0) {
        ff_free_stream(ctx, priv->st);
        priv->st = NULL;
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

static int bluelet_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    BlueletPriv *priv = ctx->priv_data;
    int ret;

    if (!priv->play)
        return AVERROR_EOF;

    ret = av_new_packet(pkt, A2DP_PERIOD_BYTES);
    if (ret < 0)
        return AVERROR(EIO);

    ret = ff_bluelet_read_buffer(priv, pkt->data, A2DP_PERIOD_BYTES);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }

    pkt->pts = AV_NOPTS_VALUE;
    pkt->size = ret;
    priv->available = false;

    return 0;
}

static int bluelet_dec_init(struct AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;

    priv->playback  = false;
    priv->available = false;
    priv->ctrl_connected = false;
    priv->data_connected = false;
    priv->codec_id  = AV_CODEC_ID_NONE;

    return ff_bluelet_init(priv, !!(ctx->flags & AVFMT_FLAG_NONBLOCK));
}

static void bluelet_dec_deinit(struct AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;

    ff_bluelet_deinit(priv);
}

static int bluelet_dec_control_message(struct AVFormatContext *ctx, int type,
                                       void *data, size_t data_size)
{
    BlueletPriv *priv = ctx->priv_data;
    struct pollfd *poll = data;
    int ret = 0;

    switch (type) {
        case AV_APP_TO_DEV_GET_CAPS_REQUEST: {
            struct AVDeviceCapabilitiesQuery *caps = data;
            BlueletPriv *priv = ctx->priv_data;

            if (!caps)
                return AVERROR(EINVAL);

            if (priv->codec_id == AV_CODEC_ID_NONE)
                return FFERROR_NOT_READY;

            caps->av_class = &bluelet_dec_cap_class;
            caps->device_context = ctx;
            av_opt_set_defaults(caps);
            return 0;
        }
        case AV_APP_TO_DEV_GET_POLLFD:
            if (!data || data_size < sizeof(struct pollfd) * 2)
                return AVERROR(EINVAL);

            if (priv->ctrl_fd > 0) {
                poll[ret].fd     = priv->ctrl_fd;
                poll[ret].events = priv->ctrl_connected ? POLLIN : POLLOUT;
                ret++;
            }

            if (priv->data_fd > 0 && !priv->available) {
                poll[ret].fd     = priv->data_fd;
                poll[ret].events = priv->data_connected ? POLLIN : POLLOUT;
                ret++;
            }
#ifdef CONFIG_UORB
            if (priv->uorb_fd > 0) {
                poll[ret].fd     = priv->uorb_fd;
                poll[ret].events = POLLIN;
                ret++;
            }
#endif

            break;
        case AV_APP_TO_DEV_POLL_AVAILABLE:
            if (poll->revents & POLLOUT) {
                if (priv->ctrl_fd == poll->fd)
                    priv->ctrl_connected = true;
                else
                    priv->data_connected = true;
            }

            if (poll->revents & POLLIN) {
                if (priv->ctrl_fd == poll->fd) {
                    int action = ff_bluelet_handle_event(priv);
                    if (action == BLUELET_ACTION_AVAILABLE) {
                        avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_BUFFER_READABLE, NULL, 0);
                    } else if (action == BLUELET_ACTION_CONFIG) {
                        avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
                    }
                } else if (priv->data_fd == poll->fd) {
                    priv->available = true;
                    avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_BUFFER_READABLE, NULL, 0);
                }
#ifdef CONFIG_UORB
                else if (priv->uorb_fd == poll->fd)
                    ff_bluelet_handle_uorb_event(priv);
#endif
            }

            break;
        case AV_APP_TO_DEV_PLAY:
            ff_bluelet_start(priv);
            priv->play = 1;
            avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);

            return 0;
        case AV_APP_TO_DEV_PAUSE:
            ff_bluelet_stop(priv);
            priv->play = 0;
            avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);

            return 0;
        case AV_APP_TO_DEV_DUMP:
            snprintf(data, data_size, "%s|%d|%d",
                     priv->server_name, priv->codec_id, priv->state);
            return 0;
        default:
            ret = AVERROR(ENOSYS);
            break;
    }

    return ret;
}

static int bluelet_dec_capbility_query_ranges(struct AVOptionRanges **ranges, void *obj,
                                              const char *key, int flags)
{
    return ff_bluelet_capbility_query_ranges(ranges, obj, key, flags);
}

static const AVClass bluelet_dec_cap_class = {
    .class_name   = "BLUELET indev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
    .query_ranges = bluelet_dec_capbility_query_ranges,
};

#define OFFSET(x) offsetof(BlueletPriv, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "frame_size",  "Set frame size",  OFFSET(frame_size),  AV_OPT_TYPE_INT,    {.i64 = 2},         1, INT_MAX, FLAGS },
    { "sample_rate", "Set sample rate", OFFSET(sample_rate), AV_OPT_TYPE_INT,    {.i64 = 48000},     1, INT_MAX, FLAGS },
    { "channels",    "Set channels",    OFFSET(channels),    AV_OPT_TYPE_INT,    {.i64 = 2},         1, INT_MAX, FLAGS },
    { "server_name", "Set server name", OFFSET(server_name), AV_OPT_TYPE_STRING, { .str = "local" }, 0, 0,       FLAGS },
    { "auto",        "Auto Play",       OFFSET(play),        AV_OPT_TYPE_INT,    {.i64 = 0},         0, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass bluelet_demuxer_class = {
    .class_name = "bluelet indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
};

AVInputFormat ff_bluelet_demuxer = {
    .name                       = "bluelet",
    .long_name                  = NULL_IF_CONFIG_SMALL("BLUELET audio input"),
    .priv_data_size             = sizeof(BlueletPriv),
    .read_header                = bluelet_read_header,
    .read_packet                = bluelet_read_packet,
    .init                       = bluelet_dec_init,
    .deinit                     = bluelet_dec_deinit,
    .control_message            = bluelet_dec_control_message,
    .read_close                 = bluelet_read_close,
    .flags                      = AVFMT_NOFILE,
    .priv_class                 = &bluelet_demuxer_class,
};
