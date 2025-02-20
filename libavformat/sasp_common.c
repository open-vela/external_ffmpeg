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

    if (header->version == 2) {
        header->pix_fmt = avio_rb32(ic->pb);
        length += sizeof(header->pix_fmt);

        header->first_pts = avio_rb64(ic->pb);
        length += sizeof(header->first_pts);

        header->first_dts = avio_rb64(ic->pb);
        length += sizeof(header->first_dts);
    }

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

    if (header->version == 2) {
        AV_WB32(header_buf + length, header->pix_fmt);
        length += sizeof(header->pix_fmt);

        AV_WB64(header_buf + length, header->first_pts);
        length += sizeof(header->first_pts);

        AV_WB64(header_buf + length, header->first_dts);
        length += sizeof(header->first_dts);
    }

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

    if (header->version == 2) {
        header->pts = avio_rb64(ic->pb);
        length += sizeof(header->pts);

        header->dts = avio_rb64(ic->pb);
        length += sizeof(header->dts);
    } else {
        header->timestamp = avio_rb64(ic->pb);
        length += sizeof(header->timestamp);
    }

    header->type = avio_rb32(ic->pb);
    length += sizeof(header->type);

    if (header->type == MKBETAG('v', 'i', 'd', 'e')) {
        header->info.video.width = avio_rb32(ic->pb);
        length += sizeof(header->info.video.width);

        header->info.video.height = avio_rb32(ic->pb);
        length += sizeof(header->info.video.height);

        header->info.video.fps = avio_rb32(ic->pb);
        length += sizeof(header->info.video.fps);
    } else {
        header->info.audio.sample_rate = avio_rb32(ic->pb);
        length += sizeof(header->info.audio.sample_rate);

        header->info.audio.channel = avio_rb32(ic->pb);
        length += sizeof(header->info.audio.channel);
    }

    if (avio_feof(ic->pb))
        return AVERROR_EOF;

    return length;
}

int ff_sasp_write_frame_header(char *frame_buf, const SASPFrameHeader *header)
{
    char *header_length_ptr = NULL;
    int length = 0;

    if (header->magic != MKBETAG('s', 'a', 's', 'p'))
        return AVERROR_INVALIDDATA;

    AV_WB32(frame_buf + length, header->magic);
    length += sizeof(header->magic);

    AV_WB16(frame_buf + length, header->version);
    length += sizeof(header->version);

    header_length_ptr = frame_buf + length;
    length += sizeof(header->header_len);

    AV_WB32(frame_buf + length, header->body_len);
    length += sizeof(header->body_len);

    AV_WB32(frame_buf + length, header->codec_id);
    length += sizeof(header->codec_id);

    AV_WB32(frame_buf + length, header->sequence);
    length += sizeof(header->sequence);

    if (header->version == 2) {
        AV_WB64(frame_buf + length, header->pts);
        length += sizeof(header->pts);

        AV_WB64(frame_buf + length, header->dts);
        length += sizeof(header->dts);
    } else {
        AV_WB64(frame_buf + length, header->timestamp);
        length += sizeof(header->timestamp);
    }

    AV_WB32(frame_buf + length, header->type);
    length += sizeof(header->type);

    if (header->type == MKBETAG('v', 'i', 'd', 'e')) {
        AV_WB32(frame_buf + length, header->info.video.width);
        length += sizeof(header->info.video.width);

        AV_WB32(frame_buf + length, header->info.video.height);
        length += sizeof(header->info.video.height);

        AV_WB32(frame_buf + length, header->info.video.fps);
        length += sizeof(header->info.video.fps);
    } else {
        AV_WB32(frame_buf + length, header->info.audio.sample_rate);
        length += sizeof(header->info.audio.sample_rate);

        AV_WB32(frame_buf + length, header->info.audio.channel);
        length += sizeof(header->info.audio.channel);
    }

    AV_WB16(header_length_ptr, length);

    return length;
}
