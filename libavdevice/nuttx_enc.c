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
 * NUTTX input and output: output
 *
 * This avdevice encoder can play audio to an NUTTX device.
 *
 * The playback period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time playback.
 */

#include <poll.h>

#include "libavformat/internal.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"

#include "nuttx.h"

static int nuttx_init(struct AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    int ret;

    ret = ff_nuttx_init(priv, s1->url);

    return ret < 0 ? ret : 1;
}

static void nuttx_deinit(struct AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;

    if (priv->lastpkt)
        av_packet_free(&priv->lastpkt);

    ff_nuttx_deinit(s1->priv_data);
}

static int nuttx_write_header(AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    AVStream *st = s1->streams[0];
    int ret;

    if (s1->nb_streams != 1 || s1->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return AVERROR(EINVAL);

    if (s1->flags & AVFMT_FLAG_NONBLOCK)
        priv->nonblock = true;

    priv->sample_rate    = st->codecpar->sample_rate;
    priv->channels       = st->codecpar->channels;
    priv->channel_layout = st->codecpar->channel_layout;
    priv->codec          = st->codecpar->codec_id;

    ret = ff_nuttx_open(s1->priv_data, true);
    if (ret >= 0)
        avpriv_set_pts_info(st, 64, 1, priv->sample_rate);

    return ret;
}

static int nuttx_write_trailer(struct AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;

    ff_nuttx_close(priv, priv->nonblock);
    return 0;
}

static int nuttx_write_lastpacket(AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    int ret;

    ret = ff_nuttx_write_data(priv, priv->lastpkt->data, priv->lastpkt->size);
    if (ret < 0)
        return ret;

    priv->lastpkt->data += ret;
    priv->lastpkt->size -= ret;
    if (priv->lastpkt->size)
        return AVERROR(EAGAIN);

    av_packet_free(&priv->lastpkt);
    return 0;
}

static int nuttx_write_packet(AVFormatContext *s1, AVPacket *pkt)
{
    NuttxPriv *priv = s1->priv_data;
    int ret;

    if (priv->pause) {
        if (priv->lastpkt)
            av_packet_free(&priv->lastpkt);
        return AVERROR_EOF;
    }

    if (priv->lastpkt)
        return nuttx_write_lastpacket(s1);

    if (!pkt)
        return 0;

    ret = ff_nuttx_write_data(priv, pkt->data, pkt->size);
    if (ret < 0)
        return ret;

    if (ret != pkt->size) {
        priv->lastpkt = av_packet_clone(pkt);
        priv->lastpkt->data += ret;
        priv->lastpkt->size -= ret;
        return AVERROR(EAGAIN);
    }

    return 0;
}

static int nuttx_control_message(struct AVFormatContext *s1, int type,
                                 void *data, size_t data_size)
{
    NuttxPriv *priv = s1->priv_data;

    switch (type) {
        case AV_APP_TO_DEV_SET_VOLUME:
            if (!data)
                return AVERROR(EINVAL);

            return ff_nuttx_set_volume(s1, priv, *((double *)data));
        case AV_APP_TO_DEV_MUTE:
            return ff_nuttx_set_mute(s1, priv, true);
        case AV_APP_TO_DEV_UNMUTE:
            return ff_nuttx_set_mute(s1, priv, false);
        case AV_APP_TO_DEV_TOGGLE_MUTE:
            return ff_nuttx_set_mute(s1, priv, !priv->mute);
        case AV_APP_TO_DEV_GET_VOLUME:
            return ff_nuttx_notify_changed(s1, priv, true);
        case AV_APP_TO_DEV_GET_MUTE:
            return ff_nuttx_notify_changed(s1, priv, false);
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
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_BUFFER_WRITABLE, NULL, 0);

            return ret;
        }
        case AV_APP_TO_DEV_PLAY: {
            priv->pause = false;
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);

            return 0;
        }
        case AV_APP_TO_DEV_PAUSE: {
            priv->pause = true;
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);

            return 0;
        }
        case AV_APP_TO_DEV_SET_PARAMETER:
            return ff_nuttx_set_parameter(priv, data);
        case AV_APP_TO_DEV_DUMP:
            snprintf(data, data_size, "%d|%d|%d|%d|%d",
                     priv->running, priv->flushing,
                     priv->period_bytes, priv->periods, dq_count(&priv->bufferq));
            return 0;
    }

    return AVERROR(ENOSYS);
}

static int nuttx_write_frame(AVFormatContext *s1, int stream_index,
                             AVFrame **frame, unsigned flags)
{
    NuttxPriv *priv = s1->priv_data;
    AVPacket pkt;

    /* ff_nuttx_open() should have accepted only supported formats */
    if ((flags & AV_WRITE_UNCODED_FRAME_QUERY))
        return av_sample_fmt_is_planar(s1->streams[stream_index]->codecpar->format) ?
               AVERROR(EINVAL) : 0;

    /* set only used fields */
    pkt.data     = (*frame)->data[0];
    pkt.size     = (*frame)->nb_samples * priv->frame_size;
    pkt.dts      = (*frame)->pkt_dts;
    pkt.duration = (*frame)->pkt_duration;
    return nuttx_write_packet(s1, &pkt);
}

static int nuttx_capbility_query_ranges(struct AVOptionRanges **ranges, void *obj,
                                        const char *key, int flags)
{
    return ff_nuttx_capbility_query_ranges(ranges, obj, key, flags, true);
}

static const AVClass nuttx_cap_class = {
    .class_name   = "NUTTX outdev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
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

    return ff_nuttx_get_device_list(device_list, true);
}

#define OFFSET(x) offsetof(NuttxPriv, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "periods",      "", OFFSET(periods),      AV_OPT_TYPE_INT, {.i64 = 4},   0, INT_MAX, FLAGS},
    { "period_bytes", "", OFFSET(period_bytes), AV_OPT_TYPE_INT, {.i64 = 0},   0, INT_MAX, FLAGS},
    { "period_time",  "", OFFSET(period_time),  AV_OPT_TYPE_INT, {.i64 = 20},  0, INT_MAX, FLAGS},
    { NULL },
};

static const AVClass nuttx_muxer_class = {
    .class_name = "NUTTX outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
};

AVOutputFormat ff_nuttx_muxer = {
    .name                       = "nuttx",
    .long_name                  = NULL_IF_CONFIG_SMALL("NUTTX audio output"),
    .priv_data_size             = sizeof(NuttxPriv),
    .audio_codec                = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE),
    .video_codec                = AV_CODEC_ID_NONE,
    .init                       = nuttx_init,
    .deinit                     = nuttx_deinit,
    .write_header               = nuttx_write_header,
    .write_packet               = nuttx_write_packet,
    .write_trailer              = nuttx_write_trailer,
    .control_message            = nuttx_control_message,
    .write_uncoded_frame        = nuttx_write_frame,
    .create_device_capabilities = nuttx_create_device_capabilities,
    .free_device_capabilities   = nuttx_free_device_capabilities,
    .get_device_list            = nuttx_get_device_list,
    .flags                      = AVFMT_NOFILE|AVFMT_TS_NONSTRICT,
    .priv_class                 = &nuttx_muxer_class,
};
