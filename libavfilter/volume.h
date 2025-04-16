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

/**
 * @file
 * audio volume for src filter
 */

#ifndef LIBAVFILTER_VOLUME_H
#define LIBAVFILTER_VOLUME_H

#include <stdint.h>

#include "libavutil/samplefmt.h"
#include "libavutil/float_dsp.h"
#include "libavutil/frame.h"

typedef struct VolumeContext {
    AVFloatDSPContext *fdsp;
    enum AVSampleFormat sample_fmt;
    int samples_align;
    double volume_last;
    double volume;

    void (*scale_samples)(uint8_t *dst, const uint8_t *src, int nb_samples,
                          int volume);
    void (*fade_samples)(int16_t *dst, const int16_t *src,
                         int nb_samples, int chs, int16_t dst_volume, int16_t src_volume);
} VolumeContext;

int volume_init(VolumeContext *vol, enum AVSampleFormat sample_fmt);
void volume_scale(VolumeContext *vol, AVFrame *frame);
int volume_set(VolumeContext *vol, double volume);
void volume_uninit(VolumeContext *vol);

#endif /* LIBAVFILTER_VOLUME_H */
