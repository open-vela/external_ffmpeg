/*
 * Raw Video Codec
 * Copyright (c) 2001 Fabrice Bellard
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
 * Raw Video Codec
 */

#ifndef AVCODEC_RAW_H
#define AVCODEC_RAW_H

#include "libavutil/pixfmt.h"

typedef struct PixelFormatTag {
    enum AVPixelFormat pix_fmt;
    unsigned int fourcc;
} PixelFormatTag;

enum PixelFormatTagLists {
    PIX_FMT_LIST_RAW,
    PIX_FMT_LIST_AVI,
    PIX_FMT_LIST_MOV,
};

#if !CONFIG_AUDIO_ONLY
const struct PixelFormatTag *avpriv_get_raw_pix_fmt_tags(void);

enum AVPixelFormat avpriv_pix_fmt_find(enum PixelFormatTagLists list,
                                       unsigned fourcc);
#else
static inline enum AVPixelFormat avpriv_pix_fmt_find(enum PixelFormatTagLists list,
                                                     unsigned fourcc) {
    return AV_PIX_FMT_NONE;
}

static inline const struct PixelFormatTag *avpriv_get_raw_pix_fmt_tags(void) {
    static const PixelFormatTag raw_pix_fmt_tags[1] = {
        { AV_PIX_FMT_NONE, 0 }
    };
    return raw_pix_fmt_tags;
}
#endif

#endif /* AVCODEC_RAW_H */
