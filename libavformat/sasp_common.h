/*
 * Simplified Abstraction Stream Protocol Common Header
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
 * Simplified Abstraction Stream Protocol Common Header
 */

#ifndef AVFORMAT_SASP_COMMON_H
#define AVFORMAT_SASP_COMMON_H

#include "avformat.h"

typedef struct SASPStreamHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint32_t video_codec_id;
    uint32_t audio_codec_id;

    /* video specific info */
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    int pix_fmt;              // for version 2.
    int64_t first_pts;        // for version 2.
    int64_t first_dts;        // for version 2.

    /* audio specific info */
    uint32_t sample_rate;
    uint32_t channel;
} SASPStreamHeader;

typedef struct SASPVideoInfo {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
} SASPVideoInfo;

typedef struct SASPAudioInfo {
    uint32_t sample_rate;
    uint32_t channel;
} SASPAudioInfo;

typedef struct SASPFrameHeader {
    uint16_t version;
    uint16_t header_len;
    uint32_t magic;
    uint32_t body_len;
    uint32_t codec_id;
    uint32_t sequence;
    uint64_t pts;             ///< frame pts: us. for version 2.
    uint64_t dts;             ///< frame dts: us. for version 2.
    uint64_t timestamp;       ///< frame timestamp: ms. for version 0.
    uint32_t type;            ///< frame type: video/audio
    union {
        SASPVideoInfo video;
        SASPAudioInfo audio;
    } info;
} SASPFrameHeader;

int ff_sasp_read_stream_header(AVFormatContext *ic, SASPStreamHeader *header);
int ff_sasp_write_stream_header(char *header_buf, const SASPStreamHeader *header);
int ff_sasp_read_frame_header(AVFormatContext *ic, SASPFrameHeader *header);
int ff_sasp_write_frame_header(char *frame_buf, const SASPFrameHeader *header);

#endif /* AVFORMAT_SASP_COMMON_H */
