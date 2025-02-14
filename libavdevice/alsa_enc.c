/*
 * ALSA input and output
 * Copyright (c) 2007 Luca Abeni ( lucabe72 email it )
 * Copyright (c) 2007 Benoit Fouet ( benoit fouet free fr )
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
 * ALSA input and output: output
 * @author Luca Abeni ( lucabe72 email it )
 * @author Benoit Fouet ( benoit fouet free fr )
 *
 * This avdevice encoder can play audio to an ALSA (Advanced Linux
 * Sound Architecture) device.
 *
 * The filename parameter is the name of an ALSA PCM device capable of
 * capture, for example "default" or "plughw:1"; see the ALSA documentation
 * for naming conventions. The empty string is equivalent to "default".
 *
 * The playback period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time playback.
 */

#include <alsa/asoundlib.h>
#include <poll.h>

#include "alsa.h"
#include "avdevice.h"
#include "libavcodec/bsf.h"
#include "libavformat/internal.h"
#include "libavformat/mux.h"
#include "libavutil/frame.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

static int audio_capbility_query_ranges(struct AVOptionRanges **ranges, void *obj,
    const char *key, int flags)
{
    return ff_audio_capbility_query_ranges(ranges, obj, key, flags, true);
}

static const AVClass alsa_cap_class = {
    .class_name = "ALSA outdev capbility",
    .item_name = av_default_item_name,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges = audio_capbility_query_ranges,
};

static av_cold int audio_write_header(AVFormatContext *s1)
{
    AlsaData *priv = s1->priv_data;
    AVStream *st = NULL;
    unsigned int sample_rate;
    enum AVCodecID codec_id;
    int res;

    if (s1->nb_streams != 1 || s1->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
        av_log(s1, AV_LOG_ERROR, "Only a single audio stream is supported.\n");
        return AVERROR(EINVAL);
    }
    st = s1->streams[0];

    sample_rate = st->codecpar->sample_rate;
    codec_id    = st->codecpar->codec_id;
    res = ff_alsa_open(s1, SND_PCM_STREAM_PLAYBACK, &sample_rate,
        st->codecpar->ch_layout.nb_channels, &codec_id);
    if (sample_rate != st->codecpar->sample_rate) {
        av_log(s1, AV_LOG_ERROR,
               "sample rate %d not available, nearest is %d\n",
               st->codecpar->sample_rate, sample_rate);
        goto fail;
    }
    avpriv_set_pts_info(st, 64, 1, sample_rate);

    priv->running = 1;
    priv->timestamp = 0;

    return res;

fail:
    snd_pcm_close(priv->h);
    return AVERROR(EIO);
}

static int audio_write_trailer(struct AVFormatContext *s1)
{
    FFStream *const sti = ffstream(s1->streams[0]);
    AlsaData *priv = s1->priv_data;

    ff_alsa_close(s1);

    priv->timestamp = 0;
    sti->cur_dts = 0;
    priv->running = 0;

    if (sti->bsfc) {
        av_bsf_flush(sti->bsfc);
        av_bsf_free(&sti->bsfc);
        sti->bitstream_checked = 0;
    }
    return 0;
}

static int audio_write_lastpacket(AVFormatContext *s1)
{
    AlsaData *priv = s1->priv_data;
    int ret;

    ret = snd_pcm_writei(priv->h, priv->lastpkt->data, priv->lastpkt->size / priv->frame_size);
    av_log(s1, AV_LOG_TRACE, "audio_write_lastpacket->snd_pcm_writei(%p, %p, %d %d)\n", priv->h, priv->lastpkt->data, priv->lastpkt->size / priv->frame_size, ret);
    if (ret < 0) {
        if (ff_alsa_xrun_recover(s1, ret) < 0)
            return AVERROR(EIO);
        ret = snd_pcm_writei(priv->h, priv->lastpkt->data, priv->lastpkt->size / priv->frame_size);
        if (ret < 0)
            return AVERROR(EAGAIN);
    }

    priv->timestamp += ret;
    priv->lastpkt->data += ret * priv->frame_size;
    priv->lastpkt->size -= ret * priv->frame_size;

    if (priv->lastpkt->size)
        return AVERROR(EAGAIN);

    av_packet_free(&priv->lastpkt);
    return 0;
}

static int audio_write_packet(AVFormatContext *s1, AVPacket *pkt)
{
    AlsaData *priv = s1->priv_data;
    int res;
    int size = pkt->size;
    const uint8_t *buf = pkt->data;

    size /= priv->frame_size;

    if (!priv->running) {
        if (priv->lastpkt)
            av_packet_free(&priv->lastpkt);
        return AVERROR_EOF;
    }

    if (priv->reorder_func) {
        if (size > priv->reorder_buf_size)
            if (ff_alsa_extend_reorder_buf(priv, size))
                return AVERROR(ENOMEM);
        priv->reorder_func(buf, priv->reorder_buf, size);
        buf = priv->reorder_buf;
    }

    if (snd_pcm_state(priv->h) == SND_PCM_STATE_PAUSED)
    {
        priv->resume_min -= size;
        if (priv->resume_min <= 0)
            snd_pcm_pause(priv->h, 0);
    }

    if (priv->lastpkt)
        return audio_write_lastpacket(s1);

    res = snd_pcm_writei(priv->h, buf, size);
    av_log(s1, AV_LOG_TRACE, "audio_write_packet->snd_pcm_writei(%p, %p, %d %d)\n", priv->h, buf, size, res);
    if (res < 0) {
        if (ff_alsa_xrun_recover(s1, res) < 0)
            return AVERROR(EIO);
        res = snd_pcm_writei(priv->h, buf, size);
        if (res < 0)
            return AVERROR(EAGAIN);
    }

    priv->timestamp += res;

    if (res != size) {
        priv->lastpkt = av_packet_clone(pkt);
        priv->lastpkt->data += res * priv->frame_size;
        priv->lastpkt->size -= res * priv->frame_size;
        return AVERROR(EAGAIN);
    }

    return 0;
}

static int audio_write_frame(AVFormatContext *s1, int stream_index,
                             AVFrame **frame, unsigned flags)
{
    AlsaData *s = s1->priv_data;
    AVPacket pkt;

    /* ff_alsa_open() should have accepted only supported formats */
    if ((flags & AV_WRITE_UNCODED_FRAME_QUERY))
        return av_sample_fmt_is_planar(s1->streams[stream_index]->codecpar->format) ?
               AVERROR(EINVAL) : 0;
    /* set only used fields */
    pkt.data     = (*frame)->data[0];
    pkt.size     = (*frame)->nb_samples * s->frame_size;
    pkt.dts      = (*frame)->pkt_dts;
    pkt.duration = (*frame)->duration;
    return audio_write_packet(s1, &pkt);
}

static int audio_control_message(struct AVFormatContext *s1, int type,
    void *data, size_t data_size)
{
    AlsaData *priv = s1->priv_data;
    snd_pcm_t *pcm = priv->h;
    snd_pcm_sw_params_t *sw_params;

    switch (type) {
        case AV_APP_TO_DEV_GET_POLLFD: {
        struct pollfd *poll = data;
            int ret;
            if (!data || data_size < sizeof(struct pollfd))
                return AVERROR(EINVAL);

            if (!pcm || !priv->running)
                return 0;

            if (snd_pcm_state(pcm) == SND_PCM_STATE_PAUSED)
                return 0;

            ret = snd_pcm_poll_descriptors(pcm, poll, 1);

            if (ret < 0)
                return 0;

            return 1;
        }
        case AV_APP_TO_DEV_POLL_AVAILABLE: {
            enum AVDevToAppMessageType t;
            snd_pcm_sframes_t avail;

            if (!pcm)
                return 0;

            if (priv->running) {
                avail = snd_pcm_avail_update(pcm);
                if (avail == -EPIPE) {
                    snd_pcm_pause(pcm, 1);
                    snd_pcm_sw_params_alloca(&sw_params);
                    snd_pcm_sw_params_current(priv->h, sw_params);
                    priv->resume_min = sw_params->avail_min;
                }
                t = AV_DEV_TO_APP_BUFFER_WRITABLE;
            } else {
                t = AV_DEV_TO_APP_BUFFER_DRAINED;
            }

            avdevice_dev_to_app_control_message(s1, t, NULL, 0);
            return 0;
        }
        case AV_APP_TO_DEV_START: {
            snd_pcm_start(pcm);
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_STATE_CHANGED, &type, 0);
            return 0;
        }
        case AV_APP_TO_DEV_STOP: {
            priv->running = 0;
            snd_pcm_close(pcm);
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_STATE_CHANGED, &type, 0);
            return 0;
        }
        case AV_APP_TO_DEV_PAUSE:
            return snd_pcm_pause(pcm, 1);
        case AV_APP_TO_DEV_PLAY:
            return snd_pcm_pause(pcm, 0);
        case AV_APP_TO_DEV_DUMP:
            snprintf(data, data_size, "%s|%d %d", "alsa", priv->running, priv->running ? snd_pcm_state(pcm) : -1);
            return 0;
    }

    return AVERROR(ENOSYS);
}

static void
audio_get_output_timestamp(AVFormatContext *s1, int stream,
    int64_t *dts, int64_t *wall)
{
    AlsaData *s  = s1->priv_data;
    snd_pcm_sframes_t delay = 0;
    *wall = av_gettime();
    snd_pcm_delay(s->h, &delay);
    *dts = s->timestamp - delay;
}

static int audio_get_device_list(AVFormatContext *h, AVDeviceInfoList *device_list)
{
    return ff_alsa_get_device_list(device_list, SND_PCM_STREAM_PLAYBACK);
}

#define OFFSET(x) offsetof(AlsaData, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "periods",      "", OFFSET(periods),      AV_OPT_TYPE_INT, {.i64 = 4},   0, INT_MAX, FLAGS},
    { "period_time",  "", OFFSET(period_time),  AV_OPT_TYPE_INT, {.i64 = 20},  0, INT_MAX, FLAGS},
    { NULL },
};

static const AVClass alsa_muxer_class = {
    .class_name     = "ALSA outdev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges   = audio_capbility_query_ranges,
};

const FFOutputFormat ff_alsa_muxer = {
    .p.name               = "alsa",
    .p.long_name          = NULL_IF_CONFIG_SMALL("ALSA audio output"),
    .priv_data_size       = sizeof(AlsaData),
    .p.audio_codec        = DEFAULT_CODEC_ID,
    .p.video_codec        = AV_CODEC_ID_NONE,
    .write_header         = audio_write_header,
    .write_packet         = audio_write_packet,
    .write_trailer        = audio_write_trailer,
    .write_uncoded_frame  = audio_write_frame,
    .control_message      = audio_control_message,
    .get_device_list      = audio_get_device_list,
    .get_output_timestamp = audio_get_output_timestamp,
    .p.flags              = AVFMT_NOFILE | AVFMT_TS_NONSTRICT | AVFMT_NOTIMESTAMPS,
    .p.priv_class         = &alsa_muxer_class,
};
