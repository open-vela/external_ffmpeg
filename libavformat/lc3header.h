/*
 * LC3 Header
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
 * LC3(Low Complexity Communication Codec) Header
 */

#ifndef AVFORMAT_LC3_H
#define AVFORMAT_LC3_H

#include <stdint.h>

#define LC3_FILE_ID (0x1C | (0xCC << 8))

typedef struct LC3_header {
    uint16_t file_id;
    uint16_t header_size;
    uint16_t srate_100hz;
    uint16_t bitrate_100bps;
    uint16_t channels;
    uint16_t frame_10us;
    uint16_t rfu;
    uint16_t nsamples_low;
    uint16_t nsamples_high;
} LC3_header;


#endif /* AVFORMAT_LC3_H */