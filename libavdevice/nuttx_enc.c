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

#include "libavformat/internal.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"

#include "nuttx.h"

static av_cold int nuttx_write_header(AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    AVStream *st = s1->streams[0];
    enum AVCodecID codec_id;
    int ret;

    priv->fd = -1;

    if (s1->nb_streams != 1 ||
        s1->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(s1, AV_LOG_ERROR, "Only a single audio stream is supported.\n");
        return AVERROR(EINVAL);
    }

    codec_id = st->codecpar->codec_id;

    if (s1->flags & AVFMT_FLAG_NONBLOCK)
        priv->nonblock = true;

    priv->sample_rate = st->codecpar->sample_rate;
    priv->channels    = st->codecpar->channels;
    priv->playback    = true;

    ret = ff_nuttx_open(s1->priv_data, s1->url, codec_id);
    if (ret >= 0)
        avpriv_set_pts_info(st, 64, 1, priv->sample_rate);

    return ret;
}

static int nuttx_write_packet(AVFormatContext *s1, AVPacket *pkt)
{
    NuttxPriv *priv = s1->priv_data;
    uint8_t *buf = pkt->data;
    int size = pkt->size;
    int ret = 0;

    while (size > 0) {
        int len;

        if (priv->buffer_pos) {
            len = FFMIN(priv->period_bytes - priv->buffer_pos, size);

            memcpy(priv->buffer + priv->buffer_pos, buf, len);
            priv->buffer_pos += len;

            if (priv->buffer_pos >= priv->period_bytes) {
                ret = ff_nuttx_write_period(priv, priv->buffer, priv->period_bytes);
                if (ret < 0)
                    goto out;
                priv->buffer_pos = 0;
            }
        } else {
            len = FFMIN(priv->period_bytes, size);

            if (size >= priv->period_bytes) {
                ret = ff_nuttx_write_period(priv, buf, priv->period_bytes);
                if (ret < 0)
                    goto out;
            } else {
                memcpy(priv->buffer, buf, len);
                priv->buffer_pos = len;
            }
        }

        buf  += len;
        size -= len;
    }

    if (pkt->dts != AV_NOPTS_VALUE)
        priv->timestamp = pkt->dts;
    priv->timestamp += pkt->duration ? pkt->duration :
                       pkt->size / priv->frame_size;
out:
    if (ret < 0)
        av_log(s1, AV_LOG_ERROR, "%s, error ret %d\n", __func__, ret);

    return ret;
}

static av_cold int nuttx_write_trailer(struct AVFormatContext *s1)
{
    return ff_nuttx_close(s1->priv_data);
}

static int nuttx_control_message(struct AVFormatContext *s1, int type,
                                 void *data, size_t data_size)
{
    NuttxPriv *priv = s1->priv_data;
    int ret = 0;

    if (!data || priv->fd < 0)
        return AVERROR(EINVAL);

    switch (type) {
        case AV_APP_TO_DEV_SET_VOLUME:
            ret = ff_nuttx_set_volume(s1, priv, *((double *)data));
            break;
        case AV_APP_TO_DEV_MUTE:
            ret = ff_nuttx_set_mute(s1, priv, true);
            break;
        case AV_APP_TO_DEV_UNMUTE:
            ret = ff_nuttx_set_mute(s1, priv, false);
            break;
        case AV_APP_TO_DEV_TOGGLE_MUTE:
            ret = ff_nuttx_set_mute(s1, priv, !priv->mute);
            break;
        case AV_APP_TO_DEV_GET_VOLUME:
            ret = ff_nuttx_notify_changed(s1, priv, true);
            break;
        case AV_APP_TO_DEV_GET_MUTE:
            ret = ff_nuttx_notify_changed(s1, priv, false);
            break;
    }

    return ret;
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

static void nuttx_get_output_timestamp(AVFormatContext *s1, int stream,
                                       int64_t *dts, int64_t *wall)
{
    NuttxPriv *priv = s1->priv_data;

    *wall = av_gettime();
    *dts  = priv->timestamp;
}

static int nuttx_capbility_query_ranges(struct AVOptionRanges **ranges, void *obj,
                                        const char *key, int flags)
{
    return ff_nuttx_capbility_query_ranges(ranges, obj, key, flags, true);
}

#define OFFSET(x) offsetof(NuttxPriv, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "periods",      "", OFFSET(periods),         AV_OPT_TYPE_INT,     {.i64 = 4},    1, INT_MAX, FLAGS},
    { "period_bytes", "", OFFSET(period_bytes),    AV_OPT_TYPE_INT,     {.i64 = 8192}, 1, INT_MAX, FLAGS},
    { NULL },
};

static const AVClass nuttx_muxer_class = {
    .class_name     = "NUTTX outdev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges   = nuttx_capbility_query_ranges,
};

static int nuttx_create_device_capabilities(struct AVFormatContext *s1, struct AVDeviceCapabilitiesQuery *caps)
{
    caps->av_class = &nuttx_muxer_class;
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

AVOutputFormat ff_nuttx_muxer = {
    .name           = "nuttx",
    .long_name      = NULL_IF_CONFIG_SMALL("NUTTX audio output"),
    .priv_data_size = sizeof(NuttxPriv),
    .audio_codec    = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE),
    .video_codec    = AV_CODEC_ID_NONE,
    .write_header   = nuttx_write_header,
    .write_packet   = nuttx_write_packet,
    .write_trailer  = nuttx_write_trailer,
    .control_message      = nuttx_control_message,
    .write_uncoded_frame  = nuttx_write_frame,
    .get_output_timestamp = nuttx_get_output_timestamp,
    .create_device_capabilities = nuttx_create_device_capabilities,
    .free_device_capabilities   = nuttx_free_device_capabilities,
    .get_device_list = nuttx_get_device_list,
    .flags           = AVFMT_NOFILE,
    .priv_class      = &nuttx_muxer_class,
};
