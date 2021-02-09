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

#include "libavformat/internal.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"

#include "avdevice.h"
#include "nuttx.h"

static av_cold int nuttx_read_header(AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    enum AVCodecID codec_id;
    AVStream *st;
    int ret;

    st = avformat_new_stream(s1, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    codec_id = s1->audio_codec_id;
    if (codec_id == AV_CODEC_ID_NONE)
        codec_id = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE);

    if (s1->flags & AVFMT_FLAG_NONBLOCK)
        priv->nonblock = true;

    priv->playback = false;

    ret = ff_nuttx_open(s1->priv_data, s1->url, codec_id);
    if (ret < 0)
        return ret;

    /* take real parameters */
    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = codec_id;
    st->codecpar->sample_rate = priv->sample_rate;
    st->codecpar->channels    = priv->channels;
    st->codecpar->frame_size  = priv->frame_size;
    avpriv_set_pts_info(st, 64, 1, 1000000);  /* 64 bits pts in us */

    /* microseconds instead of seconds, MHz instead of Hz */
    priv->timefilter = ff_timefilter_new(1000000.0 / priv->sample_rate,
                                priv->period_bytes / priv->frame_size,
                                1.5E-6);
    if (!priv->timefilter)
        goto fail;

    return 0;

fail:
    ff_nuttx_close(priv);
    return AVERROR(ENOMEM);
}

static int nuttx_read_packet(AVFormatContext *s1, AVPacket *pkt)
{
    NuttxPriv *priv = s1->priv_data;
    int64_t dts;
    int size;
    int ret;

    ret = av_new_packet(pkt, priv->period_bytes);
    if (ret < 0)
        return AVERROR(EIO);

    size = priv->period_bytes;
    ret = ff_nuttx_read_data(priv, pkt->data, &size);
    if (ret < 0) {
        av_packet_unref(pkt);

        if (ret != AVERROR(EAGAIN))
            av_log(s1, AV_LOG_ERROR, "%s, error ret %d\n", __func__, ret);

        return ret;
    }

    dts = av_gettime();
    pkt->pts = ff_timefilter_update(priv->timefilter, dts, priv->last_period);
    priv->last_period = size / priv->frame_size;

    pkt->size = size;

    return 0;
}

static int nuttx_read_close(AVFormatContext *s1)
{
    return ff_nuttx_close(s1->priv_data);
}

static int nuttx_capbility_query_ranges(struct AVOptionRanges **ranges, void *obj,
                                        const char *key, int flags)
{
    return ff_nuttx_capbility_query_ranges(ranges, obj, key, flags, false);
}

#define OFFSET(x) offsetof(NuttxPriv, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "periods",        "", OFFSET(periods),        AV_OPT_TYPE_INT, {.i64 = 4},     1, INT_MAX, FLAGS },
    { "period_bytes",   "", OFFSET(period_bytes),   AV_OPT_TYPE_INT, {.i64 = 8192},  1, INT_MAX, FLAGS },
    { "period_time",    "", OFFSET(period_time),    AV_OPT_TYPE_INT, {.i64 = 0},     0, INT_MAX, FLAGS },
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT, {.i64 = 48000}, 1, INT_MAX, FLAGS },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT, {.i64 = 1},     1, INT_MAX, FLAGS },
    { "channel_layout", "", OFFSET(channel_layout), AV_OPT_TYPE_INT, {.i64 = 0},     0, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass nuttx_demuxer_class = {
    .class_name     = "NUTTX indev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
    .query_ranges   = nuttx_capbility_query_ranges,
};

static int nuttx_create_device_capabilities(struct AVFormatContext *s1, struct AVDeviceCapabilitiesQuery *caps)
{
    caps->av_class = &nuttx_demuxer_class;
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

AVInputFormat ff_nuttx_demuxer = {
    .name           = "nuttx",
    .long_name      = NULL_IF_CONFIG_SMALL("NUTTX audio input"),
    .priv_data_size = sizeof(NuttxPriv),
    .read_header    = nuttx_read_header,
    .read_packet    = nuttx_read_packet,
    .read_close     = nuttx_read_close,
    .create_device_capabilities = nuttx_create_device_capabilities,
    .free_device_capabilities   = nuttx_free_device_capabilities,
    .get_device_list = nuttx_get_device_list,
    .flags           = AVFMT_NOFILE,
    .priv_class      = &nuttx_demuxer_class,
};
