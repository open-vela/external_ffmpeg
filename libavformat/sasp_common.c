/*
 * Simplified Abstraction Stream Protocol Common File
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
 * Simplified Abstraction Stream Protocol Common File
 */

#include "avformat.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"

#include "sasp_common.h"

int ff_sasp_read_stream_header(AVFormatContext *ic, SASPStreamHeader *header)
{
    int length = 0;

    header->magic = avio_rb32(ic->pb);
    length += sizeof(header->magic);

    if (header->magic != MKBETAG('s', 'a', 's', 'p'))
        return AVERROR_INVALIDDATA;

    header->version = avio_rb16(ic->pb);
    length += sizeof(header->version);

    header->header_len = avio_rb16(ic->pb);
    length += sizeof(header->header_len);

    header->video_codec_id = avio_rb32(ic->pb);
    length += sizeof(header->video_codec_id);

    header->audio_codec_id = avio_rb32(ic->pb);
    length += sizeof(header->audio_codec_id);

    header->width = avio_rb32(ic->pb);
    length += sizeof(header->width);

    header->height = avio_rb32(ic->pb);
    length += sizeof(header->height);

    header->fps = avio_rb32(ic->pb);
    length += sizeof(header->fps);

    header->sample_rate = avio_rb32(ic->pb);
    length += sizeof(header->sample_rate);

    header->channel = avio_rb32(ic->pb);
    length += sizeof(header->channel);

    if (avio_feof(ic->pb))
        return AVERROR_EOF;

    return length;
}

int ff_sasp_write_stream_header(char *header_buf, const SASPStreamHeader *header)
{
    int length = 0;

    if (header->magic != MKBETAG('s', 'a', 's', 'p'))
        return AVERROR_INVALIDDATA;

    AV_WB32(header_buf + length, header->magic);
    length += sizeof(header->magic);

    AV_WB16(header_buf + length, header->version);
    length += sizeof(header->version);

    AV_WB16(header_buf + length, sizeof(*header));
    length += sizeof(header->header_len);

    AV_WB32(header_buf + length, header->video_codec_id);
    length += sizeof(header->video_codec_id);

    AV_WB32(header_buf + length, header->audio_codec_id);
    length += sizeof(header->audio_codec_id);

    AV_WB32(header_buf + length, header->width);
    length += sizeof(header->width);

    AV_WB32(header_buf + length, header->height);
    length += sizeof(header->height);

    AV_WB32(header_buf + length, header->fps);
    length += sizeof(header->fps);

    AV_WB32(header_buf + length, header->sample_rate);
    length += sizeof(header->sample_rate);

    AV_WB32(header_buf + length, header->channel);
    length += sizeof(header->channel);

    return length;
}

int ff_sasp_read_frame_header(AVFormatContext *ic, SASPFrameHeader *header)
{
    int length = 0;

    header->magic = avio_rb32(ic->pb);
    length += sizeof(header->magic);

    if (header->magic != MKBETAG('s', 'a', 's', 'p'))
        return AVERROR_INVALIDDATA;

    header->version = avio_rb16(ic->pb);
    length += sizeof(header->version);

    header->header_len = avio_rb16(ic->pb);
    length += sizeof(header->header_len);

    header->body_len = avio_rb32(ic->pb);
    length += sizeof(header->body_len);

    header->codec_id = avio_rb32(ic->pb);
    length += sizeof(header->codec_id);

    header->sequence = avio_rb32(ic->pb);
    length += sizeof(header->sequence);

    header->timestamp_ms = avio_rb32(ic->pb);
    length += sizeof(header->timestamp_ms);

    header->timestamp_s = avio_rb64(ic->pb);
    length += sizeof(header->timestamp_s);

    if (avio_feof(ic->pb))
        return AVERROR_EOF;

    return length;
}

int ff_sasp_write_frame_header(char *frame_buf, const SASPFrameHeader *header)
{
    int length = 0;

    if (header->magic != MKBETAG('s', 'a', 's', 'p'))
        return AVERROR_INVALIDDATA;

    AV_WB32(frame_buf + length, header->magic);
    length += sizeof(header->magic);

    AV_WB16(frame_buf + length, header->version);
    length += sizeof(header->version);

    AV_WB16(frame_buf + length, sizeof(*header));
    length += sizeof(header->header_len);

    AV_WB32(frame_buf + length, header->body_len);
    length += sizeof(header->body_len);

    AV_WB32(frame_buf + length, header->codec_id);
    length += sizeof(header->codec_id);

    AV_WB32(frame_buf + length, header->sequence);
    length += sizeof(header->sequence);

    AV_WB32(frame_buf + length, header->timestamp_ms);
    length += sizeof(header->timestamp_ms);

    AV_WB64(frame_buf + length, header->timestamp_s);
    length += sizeof(header->timestamp_s);

    return length;
}
