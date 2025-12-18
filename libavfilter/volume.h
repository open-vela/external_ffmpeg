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

#ifndef AVFILTER_VOLUME_H
#define AVFILTER_VOLUME_H

#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/samplefmt.h"
#include "libavutil/float_dsp.h"
#include "libavutil/frame.h"

enum PrecisionType {
    PRECISION_FIXED = 0,
    PRECISION_FLOAT,
    PRECISION_DOUBLE,
};

typedef struct VolumeContext {
    AVFloatDSPContext *fdsp;
    enum AVSampleFormat sample_fmt;
    int samples_align;
    double volume_last;
    double volume;

    enum PrecisionType precision;
    AVFrame *conv_frame;
    enum AVSampleFormat mid_fmt;

    void (*scale_samples)(uint8_t *dst, const uint8_t *src, int nb_samples,
                          int volume);
    void (*fade_samples)(uint8_t *dst, const uint8_t *src,
                         int nb_samples, int chs, int16_t dst_volume, int16_t src_volume);
} VolumeContext;

int volume_init(VolumeContext *vol, enum AVSampleFormat sample_fmt, enum PrecisionType precision);
void volume_scale(VolumeContext *vol, AVFrame *frame);
void volume_set(VolumeContext *vol, double volume);
void volume_uninit(VolumeContext *vol);
int volume_parse_index_db(const char *str, int *index, double *value);

#endif /* AVFILTER_VOLUME_H */
