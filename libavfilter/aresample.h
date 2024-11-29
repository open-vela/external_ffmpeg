/*
 * Copyright (c) 2024 HiccupZhu
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

#ifndef AVFILTER_ARESAMPLE_H
#define AVFILTER_ARESAMPLE_H

#include <libavutil/channel_layout.h>

typedef struct AVClass AVClass;
struct SwrContext;
typedef struct AResampleContext {
    const AVClass *class;
    struct SwrContext *swr;
    int format;
    int sample_rate;
    AVChannelLayout ch_layout;
} AResampleContext;

typedef struct AVFilterLink AVFilterLink;
typedef struct AVFrame AVFrame;

void ff_resample_init(AResampleContext *ctx);
void ff_resample_uninit(AResampleContext *ctx);
/*
* return > 0 is converted nb_samples, 0 is no converted, < 0 is error
*/
int ff_resample_frame(AResampleContext *ar, AVFilterLink *link, AVFrame *frame, AVFrame **pframe);

int ff_resample_get_delay(AResampleContext *ar, int64_t base);

#endif /* AVFILTER_ARESAMPLE_H */
