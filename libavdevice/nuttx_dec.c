/*
 * NUTTX input and output
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
 * NUTTX input and output: input
 *
 * This avdevice decoder can capture audio from an NUTTX device.
 *
 * The capture period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time capture.
 */

#include <poll.h>

#include "libavformat/internal.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

#include "nuttx.h"

static int nuttx_init(struct AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;

    return ff_nuttx_init(priv, s1->url);
}

static void nuttx_deinit(struct AVFormatContext *s1)
{
    ff_nuttx_deinit(s1->priv_data);
}

static int nuttx_control_message(struct AVFormatContext *s1,
                                 int type, void *data, size_t data_size)
{
    NuttxPriv *priv = s1->priv_data;

    switch (type) {
        case AV_APP_TO_DEV_GET_POLLFD: {
            struct pollfd *poll = data;

            if (!data || data_size < sizeof(struct pollfd))
                return AVERROR(EINVAL);

            poll[0].fd      = priv->mq;
            poll[0].events  = POLLIN;

            return 1;
        }
        case AV_APP_TO_DEV_POLL_AVAILABLE: {
            int ret;

            ret = ff_nuttx_poll_available(priv, true);
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_BUFFER_READABLE, NULL, 0);

            return ret;
        }
        case AV_APP_TO_DEV_PLAY: {
            priv->pause = false;
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_BUFFER_READABLE, NULL, 0);

            return 0;
        }
        case AV_APP_TO_DEV_PAUSE: {
            priv->pause = true;

            return 0;
        }
        case AV_APP_TO_DEV_DUMP:
            snprintf(data, data_size, "%d|%d|%d|%d|%d",
                     priv->running, priv->flushing,
                     priv->period_bytes, priv->periods, dq_count(&priv->bufferq));
            return 0;
    }

    return AVERROR(ENOSYS);
}

static int nuttx_read_header(AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    AVStream *st;
    int ret;

    st = avformat_new_stream(s1, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    if (s1->audio_codec_id != AV_CODEC_ID_NONE)
        priv->codec = s1->audio_codec_id;

    if (priv->codec == AV_CODEC_ID_NONE)
        priv->codec = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE);

    if (s1->flags & AVFMT_FLAG_NONBLOCK)
        priv->nonblock = true;

    ret = ff_nuttx_open(s1->priv_data, false);
    if (ret < 0) {
        ff_free_stream(s1, st);
        return ret;
    }

    /* take real parameters */
    st->codecpar->codec_type     = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id       = priv->codec;
    st->codecpar->sample_rate    = priv->sample_rate;
    st->codecpar->channels       = priv->channels;
    st->codecpar->frame_size     = priv->frame_size;
    st->codecpar->channel_layout = priv->channel_layout;
    avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */

    return 0;
}

static int nuttx_read_close(AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    AVStream *st = s1->streams[0];

    ff_nuttx_close(priv, priv->nonblock);
    ff_free_stream(s1, st);

    return 0;
}

static int nuttx_read_packet(AVFormatContext *s1, AVPacket *pkt)
{
    NuttxPriv *priv = s1->priv_data;
    int ret;

    if (priv->pause)
        return AVERROR_EOF;

    ret = av_new_packet(pkt, priv->period_bytes);
    if (ret < 0)
        return ret;

    ret = ff_nuttx_read_data(priv, pkt->data, priv->period_bytes);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }

    pkt->size = ret;
    priv->captured += ret;
    pkt->pts = priv->captured * 1000000 / (priv->frame_size * priv->sample_rate);

    return 0;
}

static int nuttx_capbility_query_ranges(struct AVOptionRanges **ranges, void *obj,
                                        const char *key, int flags)
{
    return ff_nuttx_capbility_query_ranges(ranges, obj, key, flags, false);
}

static const AVClass nuttx_cap_class = {
    .class_name   = "NUTTX indev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
    .query_ranges = nuttx_capbility_query_ranges,
};

static int nuttx_create_device_capabilities(struct AVFormatContext *s1, struct AVDeviceCapabilitiesQuery *caps)
{
    if (!caps)
        return AVERROR(EINVAL);

    caps->av_class = &nuttx_cap_class;
    return 0;
}

static int nuttx_free_device_capabilities(struct AVFormatContext *s, struct AVDeviceCapabilitiesQuery *caps)
{
    return 0;
}

static int nuttx_get_device_list(struct AVFormatContext *s, struct AVDeviceInfoList *device_list)
{
    if (!device_list)
        return AVERROR(EINVAL);

    return ff_nuttx_get_device_list(device_list, false);
}

#define OFFSET(x) offsetof(NuttxPriv, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "periods",        "", OFFSET(periods),        AV_OPT_TYPE_INT,            {.i64 = 4},                0,                INT_MAX, FLAGS },
    { "period_bytes",   "", OFFSET(period_bytes),   AV_OPT_TYPE_INT,            {.i64 = 0},                0,                INT_MAX, FLAGS },
    { "period_time",    "", OFFSET(period_time),    AV_OPT_TYPE_INT,            {.i64 = 20},               0,                INT_MAX, FLAGS },
    { "codec",          "", OFFSET(codec),          AV_OPT_TYPE_INT,            {.i64 = AV_CODEC_ID_NONE}, AV_CODEC_ID_NONE, INT_MAX, FLAGS },
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT,            {.i64 = 48000},            1,                INT_MAX, FLAGS },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT,            {.i64 = 2},                1,                INT_MAX, FLAGS },
    { "channel_layout", "", OFFSET(channel_layout), AV_OPT_TYPE_CHANNEL_LAYOUT, {.i64 = 0},                0,                INT_MAX, FLAGS },
    { NULL },
};

static const AVClass nuttx_demuxer_class = {
    .class_name = "NUTTX indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
};

AVInputFormat ff_nuttx_demuxer = {
    .name                       = "nuttx",
    .long_name                  = NULL_IF_CONFIG_SMALL("NUTTX audio input"),
    .priv_data_size             = sizeof(NuttxPriv),
    .init                       = nuttx_init,
    .deinit                     = nuttx_deinit,
    .control_message            = nuttx_control_message,
    .read_header                = nuttx_read_header,
    .read_packet                = nuttx_read_packet,
    .read_close                 = nuttx_read_close,
    .create_device_capabilities = nuttx_create_device_capabilities,
    .free_device_capabilities   = nuttx_free_device_capabilities,
    .get_device_list            = nuttx_get_device_list,
    .flags                      = AVFMT_NOFILE,
    .priv_class                 = &nuttx_demuxer_class,
};
