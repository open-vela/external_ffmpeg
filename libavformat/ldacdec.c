/*
 * LDAC Audio Demuxer
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
 * LDAC Audio Demuxer
 */

#include "avformat.h"
#include "demux.h"
#include "internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "ldacBT.h"

#define SAMPLE_RATE_44100_INDEX     0
#define SAMPLE_RATE_48000_INDEX     1
#define SAMPLE_RATE_88200_INDEX     2
#define SAMPLE_RATE_96000_INDEX     3
#define SAMPLE_RATE_176400_INDEX    4
#define SAMPLE_RATE_192000_INDEX    5

#define MONO_CHANNEL_CONFIG_INDEX   0
#define DUAL_CHANNEL_CONFIG_INDEX   1
#define STEREO_CHANNEL_CONFIG_INDEX 2

#define LDAC_HEADER_SIZE            8
#define LDAC_SYNC_WORD_SIZE         1

#define FS_INFO_MASK                0x3F
#define CM_MASK                     0x07

#define SAMPLE_RATE_MASK            0x07
#define CHANNEL_CONFIG_MASK         0x03

#define MQ_SYNC_WORD_INDEX          55
#define SQ_SYNC_WORD_INDEX          110
#define HQ_SYNC_WORD_INDEX          165

#define LDAC_BUFFER_SIZE            400

typedef struct LDACContext {
    const AVClass *class; ///< class for private options
    char *sfmt_str;
} LDACContext;


static int ldac_probe(const AVProbeData *p)
{
    if (p->buf[0] == LDACBT_VENDOR_ID0 &&
        p->buf[1] == LDACBT_VENDOR_ID1 &&
        p->buf[2] == LDACBT_VENDOR_ID2 &&
        p->buf[3] == LDACBT_VENDOR_ID3 &&
        p->buf[4] == LDACBT_CODEC_ID0  &&
        p->buf[5] == LDACBT_CODEC_ID1) {
        return AVPROBE_SCORE_MAX;
    } else if (p->buf[0] == 0xAA) {
        return AVPROBE_SCORE_MAX;
    } else
        return 0;
}

static int find_frame_len(unsigned char *buffer, int size, int channels)
{
    if(size <= HQ_SYNC_WORD_INDEX * channels)
        return 0;

    if (buffer[MQ_SYNC_WORD_INDEX * channels] == 0xAA)
        return MQ_SYNC_WORD_INDEX * channels;
    else if (buffer[SQ_SYNC_WORD_INDEX * channels] == 0xAA)
        return SQ_SYNC_WORD_INDEX * channels;
    else if (buffer[HQ_SYNC_WORD_INDEX * channels] == 0xAA)
        return HQ_SYNC_WORD_INDEX * channels;

    return 0;
}

static int parse_header(AVFormatContext *s, int *sf, int *channels, int *frame_size)
{
    int ret = AVERROR(EINVAL);
    unsigned char *buffer;
    int buffer_offset;
    int buffer_size;

    buffer = av_mallocz(LDAC_BUFFER_SIZE);
    if (!buffer)
        return AVERROR(ENOMEM);

    if (ret = avio_read(s->pb, buffer, LDAC_BUFFER_SIZE) < 0)
        goto fail;

    if (buffer[0] == 0xAA) {
        avio_seek(s->pb, 0, SEEK_SET);

        /* get sampling frequency */
        switch ((buffer[1] >> 5) & SAMPLE_RATE_MASK) {
        case SAMPLE_RATE_44100_INDEX:  *sf = 44100;  break;
        case SAMPLE_RATE_48000_INDEX:  *sf = 48000;  break;
        case SAMPLE_RATE_88200_INDEX:  *sf = 88200;  break;
        case SAMPLE_RATE_96000_INDEX:  *sf = 96000;  break;
        case SAMPLE_RATE_176400_INDEX: *sf = 176400; break;
        case SAMPLE_RATE_192000_INDEX: *sf = 192000; break;
        default:
            av_log(s, AV_LOG_ERROR, "Unsupported Sampling Frequency Info 0x%02x!\n",
                   (buffer[1] >> 5) & SAMPLE_RATE_MASK);
            goto fail;
        }

        /* get channel mode & channel */
        switch ((buffer[1] >> 3) & CHANNEL_CONFIG_MASK) {
        case MONO_CHANNEL_CONFIG_INDEX:
            *channels = 1;
            break;
        case DUAL_CHANNEL_CONFIG_INDEX:
        case STEREO_CHANNEL_CONFIG_INDEX:
            *channels = 2;
            break;
        default:
            av_log(s, AV_LOG_ERROR, "Unsupported Channel Info 0x%02x!\n",
                   (buffer[1] >> 3) & CHANNEL_CONFIG_MASK);
            goto fail;
        }

        buffer_offset = 0;
        buffer_size  = LDAC_BUFFER_SIZE - LDAC_SYNC_WORD_SIZE;
    } else {
        avio_seek(s->pb, 8, SEEK_SET);

        if (buffer[6] & LDACBT_SAMPLING_FREQ_192000)
            *sf = 192000;
        else if (buffer[6] & LDACBT_SAMPLING_FREQ_176400)
            *sf = 176400;
        else if (buffer[6] & LDACBT_SAMPLING_FREQ_096000)
            *sf = 96000;
        else if (buffer[6] & LDACBT_SAMPLING_FREQ_088200)
            *sf = 88200;
        else if (buffer[6] & LDACBT_SAMPLING_FREQ_048000)
            *sf = 48000;
        else if (buffer[6] & LDACBT_SAMPLING_FREQ_044100)
            *sf = 44100;
        else {
            av_log(s, AV_LOG_ERROR, "Unsupported FS INFO 0x%02x!\n", buffer[6] & FS_INFO_MASK);
            goto fail;
        }

        /* get channel mode & channel */
        switch (buffer[7] & CM_MASK) {
        case LDACBT_CHANNEL_MODE_MONO:
            *channels = 1;
            break;
        case LDACBT_CHANNEL_MODE_DUAL_CHANNEL:
        case LDACBT_CHANNEL_MODE_STEREO:
            *channels = 2;
            break;
        default:
            av_log(s, AV_LOG_ERROR, "Unsupported Channel Mode 0x%02x!\n", buffer[7] & CM_MASK);
            goto fail;
        }

        buffer_offset = LDAC_HEADER_SIZE;
        buffer_size  = LDAC_BUFFER_SIZE - LDAC_HEADER_SIZE;
    }

    *frame_size = find_frame_len(buffer + buffer_offset, buffer_size, *channels);
    if (*frame_size == 0)
        goto fail;

    av_freep(&buffer);
    return 0;

fail:
    av_freep(&buffer);
    return ret;
}

static int ldac_read_header(AVFormatContext *s)
{
    LDACContext *ldac = s->priv_data;
    AVStream *st;
    int sample_rate;
    int channels;
    int frame_size;
    int ret;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    if (ret = parse_header(s, &sample_rate, &channels, &frame_size) < 0)
        return ret;

    st->codecpar->format = AV_SAMPLE_FMT_S16;
    if (ldac->sfmt_str != NULL) {
        if (!av_strcasecmp(ldac->sfmt_str, "s32"))
            st->codecpar->format = AV_SAMPLE_FMT_S32;
        else if (!av_strcasecmp(ldac->sfmt_str, "flt"))
            st->codecpar->format = AV_SAMPLE_FMT_FLT;
    }

    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_LDAC;
    st->codecpar->sample_rate = sample_rate;
    st->codecpar->frame_size  = frame_size;
    av_channel_layout_default(&st->codecpar->ch_layout, channels);
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    return 0;
}

static int ldac_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st = s->streams[0];

    if (avio_feof(s->pb))
        return AVERROR_EOF;

    return av_get_packet(s->pb, pkt, st->codecpar->frame_size);
}

static const AVOption ldac_options[] = {
    {"sfmt", "ldac pcm sample format", offsetof(LDACContext, sfmt_str), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM},
    {NULL},
};

static const AVClass ldac_class = {
    .class_name = "ldac",
    .item_name  = av_default_item_name,
    .option     = ldac_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_ldac_demuxer = {
    .p.name         = "ldac",
    .p.long_name    = NULL_IF_CONFIG_SMALL("LDAC Audio Demuxer"),
    .p.priv_class   = &ldac_class,
    .priv_data_size = sizeof(LDACContext),
    .read_probe     = ldac_probe,
    .read_header    = ldac_read_header,
    .read_packet    = ldac_read_packet,
    .p.flags        = AVFMT_GENERIC_INDEX,
    .p.extensions   = "ldac",
    .p.mime_type    = "audio/ldac",
    .raw_codec_id   = AV_CODEC_ID_LDAC,
};
