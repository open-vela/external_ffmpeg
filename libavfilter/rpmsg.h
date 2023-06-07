/*
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

#ifndef AVFILTER_RPMSG_H
#define AVFILTER_RPMSG_H

#define RPMSG_FORMAT_NEG              1
#define RPMSG_FRAME_REQ               2
#define RPMSG_FRAME_ACK               3
#define RPMSG_FRAME_EOF               4

#pragma pack(push,4)

typedef struct RpmsgFormatInfo {
    uint32_t sample_rate;
    uint32_t sample_fmt;
    uint32_t codec_id;
    uint64_t nb_channels;
    uint64_t order_channels;
} RpmsgFormatInfo;

typedef struct RpmsgFrameInfo {
    uint32_t length;
    uint32_t offset;
} RpmsgFrameInfo;

typedef struct RpmsgInfo {
    uint32_t flag;
    union {
        RpmsgFrameInfo  frm;
        RpmsgFormatInfo fmt;
    } u;
} RpmsgInfo;

#pragma pack(pop)

#endif /* AVFILTER_RPMSG_H */
