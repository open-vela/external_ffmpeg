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
 * audio volume filter
 */

#ifndef AVFILTER_VOLUME_H
#define AVFILTER_VOLUME_H

#include <stdint.h>
#include <stdbool.h>
#include "libavutil/eval.h"
#include "libavutil/float_dsp.h"
#include "libavutil/log.h"
#include "libavutil/samplefmt.h"

enum PrecisionType {
    PRECISION_FIXED = 0,
    PRECISION_FLOAT,
    PRECISION_DOUBLE,
};

enum EvalMode {
    EVAL_MODE_ONCE,
    EVAL_MODE_FRAME,
    EVAL_MODE_NB
};

enum VolumeVarName {
    VAR_N,
    VAR_NB_CHANNELS,
    VAR_NB_CONSUMED_SAMPLES,
    VAR_NB_SAMPLES,
    VAR_POS,
    VAR_PTS,
    VAR_SAMPLE_RATE,
    VAR_STARTPTS,
    VAR_STARTT,
    VAR_T,
    VAR_TB,
    VAR_VOLUME,
    VAR_VARS_NB
};

enum ReplayGainType {
    REPLAYGAIN_DROP,
    REPLAYGAIN_IGNORE,
    REPLAYGAIN_TRACK,
    REPLAYGAIN_ALBUM,
};

typedef struct VolumeContext {
    const AVClass *class;
    AVFloatDSPContext *fdsp;
    int precision;
    int eval_mode;
    const char *volume_expr;
    AVExpr *volume_pexpr;
    double var_values[VAR_VARS_NB];

    int replaygain;
    double replaygain_preamp;
    int    replaygain_noclip;
    double volume;
    int    volume_i;
    int    channels;
    int    planes;
    enum AVSampleFormat sample_fmt;

    void (*scale_samples)(uint8_t *dst, const uint8_t *src, int nb_samples,
                          int volume);

    /**
     * @brief Function pointer for fading samples.
     *
     * @param dst Destination buffer for faded samples.
     * @param src Source buffer for original samples.
     * @param nb_samples Number of samples to fade.
     * @param chs Number of channels in the audio samples.
     * @param dst_volume Destination volume level for fading.
     * @param src_volume Source volume level for fading.
     */
    void (*fade_samples)(uint8_t *dst, const uint8_t *src, int nb_samples, int chs,
                         int dst_volume, int src_volume);
    int samples_align;

    /**
     * @brief Flag indicating whether fading is in progress.
     *
     * If this flag is set to true, it means that the audio samples are currently being faded.
     */
    bool voluming;

    /**
     * @brief Source volume level for fading.
     *
     * This variable stores the backup volume level for fading. It is used to calculate the
     * step size when adjusting the volume.
     */
    int  volume_isrc;
} VolumeContext;

void ff_volume_init_x86(VolumeContext *vol);

#endif /* AVFILTER_VOLUME_H */
