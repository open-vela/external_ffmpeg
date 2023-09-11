/*
 * Simplified Abstraction Stream Protocol Muxer
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
 * Simplified Abstraction Stream Protocol Muxer
 */

#include <stdint.h>

#include "config_components.h"
#include "libavformat/internal.h"
#include "libavutil/intreadwrite.h"

#include "sasp_common.h"

static const AVCodecTag codec_sasp_tags[] = {
    { AV_CODEC_ID_MPEG4,        MKTAG('m', 'p', '4', 'v') },
    { AV_CODEC_ID_H264,         MKTAG('a', 'v', 'c', '1') },
    { AV_CODEC_ID_H264,         MKTAG('a', 'v', 'c', '3') },
    { AV_CODEC_ID_HEVC,         MKTAG('h', 'e', 'v', '1') },
    { AV_CODEC_ID_HEVC,         MKTAG('h', 'v', 'c', '1') },
    { AV_CODEC_ID_RAWVIDEO,     MKTAG('I', '4', '2', '0') },
    { AV_CODEC_ID_MJPEG,        MKTAG('m', 'p', '4', 'v') },
    { AV_CODEC_ID_PNG,          MKTAG('m', 'p', '4', 'v') },
    { AV_CODEC_ID_AAC,          MKTAG('m', 'p', '4', 'a') },
    { AV_CODEC_ID_MP3,          MKTAG('m', 'p', '4', 'a') },
    { AV_CODEC_ID_OPUS,         MKTAG('O', 'p', 'u', 's') },
    { AV_CODEC_ID_PCM_ALAW,     MKTAG('a', 'l', 'a', 'w') },
    { AV_CODEC_ID_NONE,               0 },
};

static const AVCodecTag *const sasp_codec_tags_list[] = { codec_sasp_tags, NULL };

static int sasp_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    SASPFrameHeader header = {0};
    header.magic = MKBETAG('s', 'a', 's', 'p');
    header.timestamp_ms = pkt->pts * av_q2d(s->streams[pkt->stream_index]->time_base) * 1000;
    header.body_len = pkt->size;
    header.codec_id = s->streams[pkt->stream_index]->codecpar->codec_id;
    header.header_len = sizeof(SASPFrameHeader);

    if (AVMEDIA_TYPE_AUDIO == s->streams[pkt->stream_index]->codecpar->codec_type) {
        header.type = MKBETAG('a', 'u', 'd', 'i');
        header.info.audio.channel = s->streams[pkt->stream_index]->codecpar->channels;
        header.info.audio.sample_rate = s->streams[pkt->stream_index]->codecpar->sample_rate;
    } else {
        if (pkt->flags & AV_PKT_FLAG_KEY) {
            header.type = MKBETAG('I', 'f', 'r', 'm');
        } else {
            header.type = MKBETAG('P', 'f', 'r', 'm');
        }
        header.info.video.width = s->streams[pkt->stream_index]->codecpar->width;
        header.info.video.height = s->streams[pkt->stream_index]->codecpar->height;
    }
    avio_write(s->pb, (unsigned char*)&header, header.header_len);
    avio_write(s->pb, pkt->data, pkt->size);
    return 0;
}

const AVOutputFormat ff_sasp_muxer = {
    .name = "sasp",
    .long_name   = NULL_IF_CONFIG_SMALL("Simplified Abstraction Stream Protocol Muxer"),
    .audio_codec = AV_CODEC_ID_OPUS,
    .video_codec = CONFIG_LIBX264_ENCODER ? AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG4,
    .write_packet  = sasp_write_packet,
    .codec_tag     = sasp_codec_tags_list,
    .extensions    = "sasp",
};
