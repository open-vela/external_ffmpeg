/*
 * UORB output
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
 * UORB output
 *
 * This avdevice encoder can play audio to an UORB device.
 */

#include <errno.h>
#include <poll.h>

#include <uORB/uORB.h>
#include <media/pcm16.h>

#include "libavdevice/avdevice.h"
#include "libavformat/internal.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"

typedef struct UorbPriv {
    AVClass             *class;

    int                 uorb;
    int                 play;
    int                 enable;

    enum AVSampleFormat sample_fmt;
    int                 frame_size;   ///< bytes per sample * channels
    int                 buffer_time;
    uint32_t            sample_rate;
    uint32_t            channels;
    uint64_t            channel_layout;
} UorbPriv;

static inline const struct orb_metadata *uorb_get_metadata(int sample_fmt, int ch)
{
    assert(sample_fmt == AV_SAMPLE_FMT_S16);

    switch (ch) {
        case 1: return ORB_ID(media_pcm16_1ch);
        case 2: return ORB_ID(media_pcm16_2ch);
        case 3: return ORB_ID(media_pcm16_3ch);
        case 4: return ORB_ID(media_pcm16_4ch);
        case 5: return ORB_ID(media_pcm16_5ch);
        case 6: return ORB_ID(media_pcm16_6ch);
        case 7: return ORB_ID(media_pcm16_7ch);
        case 8: return ORB_ID(media_pcm16_8ch);
    }

    return NULL;
}

static int uorb_init(struct AVFormatContext *s1)
{
    UorbPriv *priv = s1->priv_data;
    const struct orb_metadata *meta;

    meta = uorb_get_metadata(priv->sample_fmt, priv->channels);
    if (!meta)
        return AVERROR(EINVAL);

    priv->uorb = orb_advertise_queue(meta, NULL,
                 priv->buffer_time * priv->sample_rate / 1000);
    if (priv->uorb < 0)
        return AVERROR(errno);

    return 1;
}

static void uorb_deinit(struct AVFormatContext *s1)
{
    UorbPriv *priv = s1->priv_data;

    if (priv->uorb >= 0)
        orb_unadvertise(priv->uorb);
}

static int uorb_write_header(AVFormatContext *s1)
{
    UorbPriv *priv = s1->priv_data;
    AVStream *st = s1->streams[0];

    if (s1->nb_streams != 1 ||
        s1->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return AVERROR(EINVAL);

    if (priv->sample_fmt != st->codecpar->format ||
        priv->channels   != st->codecpar->channels)
        return AVERROR(EINVAL);

    if (!priv->enable || !priv->play)
        return AVERROR_EOF;

    priv->frame_size = av_get_bytes_per_sample(priv->sample_fmt) * priv->channels / 8;
    avpriv_set_pts_info(st, 64, 1, priv->sample_rate);

    return 0;
}

static int uorb_write_trailer(struct AVFormatContext *s1)
{
    return 0;
}

static int uorb_write_packet(AVFormatContext *s1, AVPacket *pkt)
{
    UorbPriv *priv = s1->priv_data;

    if (!pkt)
        return 0;

    if (!priv->enable || !priv->play)
        return AVERROR_EOF;

    return orb_publish_multi(priv->uorb, pkt->data, pkt->size);
}

static int uorb_control_message(struct AVFormatContext *s1, int type,
                                 void *data, size_t data_size)
{
    UorbPriv *priv = s1->priv_data;

    switch (type) {
        case AV_APP_TO_DEV_GET_POLLFD: {
            struct pollfd *poll = data;

            if (!data || data_size < sizeof(struct pollfd) * 2)
                return AVERROR(EINVAL);

            poll[0].fd      = priv->uorb;
            poll[0].events  = POLLPRI;
            poll[1].fd      = 0;
            poll[1].events  = 0;

            return 0;
        }
        case AV_APP_TO_DEV_POLL_AVAILABLE: {
            struct orb_state state;
            int ret;

            ret = orb_get_state(priv->uorb, &state);
            if (ret < 0)
                return ret;

            priv->enable = state.enable;

            if (state.enable)
                avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_BUFFER_WRITABLE, NULL, 0);

            return 0;
        }
        case AV_APP_TO_DEV_PLAY: {
            priv->play = 1;
            avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_BUFFER_WRITABLE, NULL, 0);
            return 0;
        }
        case AV_APP_TO_DEV_PAUSE: {
            priv->play = 0;
            return 0;
        }
    }

    return AVERROR(ENOSYS);
}

static int uorb_write_frame(AVFormatContext *s1, int stream_index,
                             AVFrame **frame, unsigned flags)
{
    UorbPriv *priv = s1->priv_data;
    AVPacket pkt;

    /* ff_uorb_open() should have accepted only supported formats */
    if ((flags & AV_WRITE_UNCODED_FRAME_QUERY))
        return av_sample_fmt_is_planar(s1->streams[stream_index]->codecpar->format) ?
               AVERROR(EINVAL) : 0;

    /* set only used fields */
    pkt.data     = (*frame)->data[0];
    pkt.size     = (*frame)->nb_samples * priv->frame_size;
    pkt.dts      = (*frame)->pkt_dts;
    pkt.duration = (*frame)->pkt_duration;
    return uorb_write_packet(s1, &pkt);
}

static int uorb_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                       const char *key, int flags)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    struct AVFormatContext *s1 = devcap->device_context;
    UorbPriv *priv = s1->priv_data;
    struct AVOptionRanges *ranges;

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        goto err;

    if (!strcmp(key, "sample_fmts")) {
        ranges->nb_components = 1;
        ranges->nb_ranges = 1;
        ranges->range = av_mallocz(sizeof(AVOptionRange *));
        if (!ranges->range)
            goto err;

        ranges->range[0] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[0])
            goto err;

        ranges->range[0]->is_range  = 0;
        ranges->range[0]->value_min = priv->sample_fmt;
        ranges->range[0]->value_max = priv->sample_fmt;
    } else if (!strcmp(key, "channel_layout")) {
        ranges->nb_components = 1;
        ranges->nb_ranges = 1;
        ranges->range = av_mallocz(sizeof(AVOptionRange *));
        if (!ranges->range)
            goto err;

        ranges->range[0] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[0])
            goto err;

        ranges->range[0]->is_range  = 0;
        ranges->range[0]->value_min = priv->channel_layout;
        ranges->range[0]->value_max = priv->channel_layout;
    } else if (!strcmp(key, "sample_rates")) {
        ranges->nb_components = 1;
        ranges->nb_ranges = 1;
        ranges->range = av_mallocz(sizeof(AVOptionRange *));
        if (!ranges->range)
            goto err;

        ranges->range[0] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[0])
            goto err;

        ranges->range[0]->is_range  = 0;
        ranges->range[0]->value_min = priv->sample_rate;
        ranges->range[0]->value_max = priv->sample_rate;
    } else {
        goto err;
    }

    *ranges_ = ranges;
    return ranges->nb_components;

err:
    av_opt_freep_ranges(&ranges);
    return AVERROR(ENOMEM);
}

static const AVClass uorb_cap_class = {
    .class_name   = "UORB outdev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges = uorb_capbility_query_ranges,
};

static int uorb_create_device_capabilities(struct AVFormatContext *s1, struct AVDeviceCapabilitiesQuery *caps)
{
    if (!caps)
        return AVERROR(EINVAL);

    caps->av_class = &uorb_cap_class;
    return 0;
}

static int uorb_free_device_capabilities(struct AVFormatContext *s, struct AVDeviceCapabilitiesQuery *caps)
{
    return 0;
}

static int uorb_get_device_list(struct AVFormatContext *s, struct AVDeviceInfoList *device_list)
{
    return 0;
}

#define OFFSET(x) offsetof(UorbPriv, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "sample_fmt",     "", OFFSET(sample_fmt),     AV_OPT_TYPE_SAMPLE_FMT,     {.i64 = AV_SAMPLE_FMT_S16}, AV_SAMPLE_FMT_U8, AV_SAMPLE_FMT_S64P, FLAGS },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT,            {.i64 = 1},                 0, INT_MAX, FLAGS},
    { "channel_layout", "", OFFSET(channel_layout), AV_OPT_TYPE_CHANNEL_LAYOUT, {.i64 = AV_CH_LAYOUT_MONO}, 0, INT_MAX, FLAGS},
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT,            {.i64 = 16000},             0, INT_MAX, FLAGS},
    { "buffer_time",    "", OFFSET(buffer_time),    AV_OPT_TYPE_INT,            {.i64 = 80},                0, INT_MAX, FLAGS},
    { "auto",           "", OFFSET(play),           AV_OPT_TYPE_INT,            {.i64 = 1},                 0, INT_MAX, FLAGS},
    { NULL },
};

static const AVClass uorb_muxer_class = {
    .class_name = "UORB outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
};

AVOutputFormat ff_uorb_muxer = {
    .name                       = "uorb",
    .long_name                  = NULL_IF_CONFIG_SMALL("UORB audio output"),
    .priv_data_size             = sizeof(UorbPriv),
    .audio_codec                = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE),
    .video_codec                = AV_CODEC_ID_NONE,
    .init                       = uorb_init,
    .deinit                     = uorb_deinit,
    .write_header               = uorb_write_header,
    .write_packet               = uorb_write_packet,
    .write_trailer              = uorb_write_trailer,
    .control_message            = uorb_control_message,
    .write_uncoded_frame        = uorb_write_frame,
    .create_device_capabilities = uorb_create_device_capabilities,
    .free_device_capabilities   = uorb_free_device_capabilities,
    .get_device_list            = uorb_get_device_list,
    .flags                      = AVFMT_NOFILE|AVFMT_TS_NONSTRICT,
    .priv_class                 = &uorb_muxer_class,
};
