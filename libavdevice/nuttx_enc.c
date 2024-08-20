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
#include "libavformat/mux.h"
#include "libavcodec/bsf.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

#include "nuttx.h"

static int nuttx_capbility_query_ranges(struct AVOptionRanges **ranges, void *obj,
                                        const char *key, int flags)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    struct AVFormatContext *s1 = devcap->device_context;

    return ff_nuttx_capbility_query_ranges(ranges, s1->url, key, flags, true);
}

static const AVClass nuttx_cap_class = {
    .class_name   = "NUTTX outdev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges = nuttx_capbility_query_ranges,
};

static int nuttx_init(struct AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    int ret;

    ret = ff_nuttx_init(priv, s1->url, true);
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

    priv->codec       = st->codecpar->codec_id;
    priv->sample_rate = st->codecpar->sample_rate;
    priv->format      = st->codecpar->format;
    priv->timestamp   = 0;
    av_channel_layout_copy(&priv->ch_layout, &st->codecpar->ch_layout);

    ret = ff_nuttx_open(s1->priv_data);
    if (ret >= 0)
        avpriv_set_pts_info(st, 64, 1, priv->sample_rate);

    return ret;
}

static int nuttx_write_trailer(struct AVFormatContext *s1)
{
    FFStream *const sti = ffstream(s1->streams[0]);
    NuttxPriv *priv = s1->priv_data;

    ff_nuttx_close(priv);
    priv->timestamp = 0;
    if (sti->bsfc) {
        av_bsf_flush(sti->bsfc);
        av_bsf_free(&sti->bsfc);
        sti->bitstream_checked = 0;
    }
    return 0;
}

static int nuttx_write_lastpacket(AVFormatContext *s1)
{
    NuttxPriv *priv = s1->priv_data;
    int ret;

    ret = ff_nuttx_write_data(priv, priv->lastpkt->data, priv->lastpkt->size);
    if (ret < 0)
        return ret;

    priv->timestamp += ret / priv->sample_bytes;

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

    if (priv->stopped) {
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

    priv->timestamp += ret / priv->sample_bytes;

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
    AVPacket pkt = {0};

    switch (type) {
        case AV_APP_TO_DEV_GET_CAPS_REQUEST: {
            struct AVDeviceCapabilitiesQuery *caps = data;

            if (!caps)
                return AVERROR(EINVAL);

            caps->av_class = &nuttx_cap_class;
            caps->device_context = s1;
            av_opt_set_defaults(caps);
            return 0;
        }
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
            snprintf(data, data_size, "vol:%f", priv->volume);
            return ff_nuttx_notify_changed(s1, priv, true);
        case AV_APP_TO_DEV_GET_MUTE:
            return ff_nuttx_notify_changed(s1, priv, false);
        case AV_APP_TO_DEV_GET_POSITION: {
            long ret = ff_nuttx_get_position(priv);
            if (ret < 0)
                return ret;
            snprintf(data, data_size, "%ld", ret);
            return 0;
        }
        case AV_APP_TO_DEV_GET_POLLFD: {
            struct pollfd *poll = data;

            if (!data || data_size < sizeof(struct pollfd))
                return AVERROR(EINVAL);

            poll[0].fd     = priv->mq;
            poll[0].events = POLLIN;

            return 1;
        }
        case AV_APP_TO_DEV_POLL_AVAILABLE: {
            enum AVDevToAppMessageType t;
            int ret;

            ret = ff_nuttx_poll_available(priv);
            t = ret == AVERROR_EXIT ? AV_DEV_TO_APP_BUFFER_DRAINED: AV_DEV_TO_APP_BUFFER_WRITABLE;

            avdevice_dev_to_app_control_message(s1, t, NULL, 0);
            return 0;
        }
        case AV_APP_TO_DEV_START: {
            priv->stopped = false;
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_STATE_CHANGED, &type, 0);

            return 0;
        }
        case AV_APP_TO_DEV_STOP: {
            priv->stopped = true;
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_STATE_CHANGED, &type, 0);

            return 0;
        }
        case AV_APP_TO_DEV_PAUSE:
            return ff_nuttx_pause(priv);
        case AV_APP_TO_DEV_PLAY:
            return ff_nuttx_resume(priv);
        case AV_APP_TO_DEV_FLUSH:
            /* drop lastpkt */
            if (priv->lastpkt)
                av_packet_free(&priv->lastpkt);

            return ff_nuttx_flush(priv);
        case AV_APP_TO_DEV_SET_PARAMETER:
            return ff_nuttx_set_parameter(priv, data);
        case AV_APP_TO_DEV_DRAIN:
            if (ff_nuttx_get_position(priv) < 0)
                return AVERROR(ENOTSUP);

            pkt.flags = AV_PKT_FLAG_EVT_EOS;
            return nuttx_write_packet(s1, &pkt);
        case AV_APP_TO_DEV_DUMP:
            snprintf(data, data_size, "%d|%d|%d|%d|%zu|%d",
                     priv->running, priv->draining,
                     priv->period_bytes, priv->periods, dq_count(&priv->bufferq), priv->mq);
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
    pkt.size     = (*frame)->nb_samples * priv->sample_bytes;
    pkt.dts      = (*frame)->pkt_dts;
    pkt.duration = (*frame)->pkt_duration;
    return nuttx_write_packet(s1, &pkt);
}

static void nuttx_get_output_timestamp(struct AVFormatContext *s1, int stream,
                                       int64_t *ts, int64_t *lat)
{
    NuttxPriv *priv = s1->priv_data;
    long latency;

    latency = ff_nuttx_get_latency(priv);
    latency = FFMAX(latency, 0);

    if (priv->lastpkt)
        latency += priv->lastpkt->size / priv->sample_bytes;

    *lat = latency;
    *ts = priv->timestamp - latency;
}

static int nuttx_get_device_list(struct AVFormatContext *s, struct AVDeviceInfoList *device_list)
{
    if (!device_list)
        return AVERROR(EINVAL);

    return ff_nuttx_get_device_list(device_list, true);
}

static int nuttx_check_bitstream(struct AVFormatContext *s, struct AVStream *st, const AVPacket *pkt)
{
    const struct CodecBsf {
        enum AVCodecID id;
        const char     *name;
    } codec_bsf_table[] = {
        {AV_CODEC_ID_AAC,  "aac_rawtoadts"},
        {AV_CODEC_ID_OPUS, "raw2tlv"}
    };

    for (size_t i = 0; i < FF_ARRAY_ELEMS(codec_bsf_table); i++) {
        if (codec_bsf_table[i].id == st->codecpar->codec_id) {
            av_log(s, AV_LOG_DEBUG, "%s bsf is added\n", codec_bsf_table[i].name);
            return ff_stream_add_bitstream_filter(st, codec_bsf_table[i].name, NULL);
        }
    }

    return 0;
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

const AVOutputFormat ff_nuttx_muxer = {
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
    .get_output_timestamp       = nuttx_get_output_timestamp,
    .get_device_list            = nuttx_get_device_list,
    .check_bitstream            = nuttx_check_bitstream,
    .flags                      = AVFMT_NOFILE | AVFMT_TS_NONSTRICT | AVFMT_NOTIMESTAMPS,
    .priv_class                 = &nuttx_muxer_class,
};
