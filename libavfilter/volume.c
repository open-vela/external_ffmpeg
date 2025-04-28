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

#include "libavutil/mem.h"

#include "volume.h"

static inline void fade_samples_s16_small(int16_t *dst, const int16_t *src,
                                          int nb_samples, int chs, int16_t dst_volume, int16_t src_volume)
{
    int i, j, k = 0;
    int32_t step;

    step = ((int32_t)(dst_volume - src_volume) * (1 << 15)) / nb_samples;
    for (i = 0; i < nb_samples; i++) {
        for (j = 0; j < chs; j++, k++) {
            dst[k] = av_clip_int16((src[k] * (src_volume + (step * i >> 15)) + 0x4000) >> 15);
        }
    }
}

static inline void scale_samples_u8(uint8_t *dst, const uint8_t *src,
                                    int nb_samples, int volume)
{
    int i;
    for (i = 0; i < nb_samples; i++)
        dst[i] = av_clip_uint8(((((int64_t)src[i] - 128) * volume + 128) >> 8) + 128);
}

static inline void scale_samples_u8_small(uint8_t *dst, const uint8_t *src,
                                          int nb_samples, int volume)
{
    int i;
    for (i = 0; i < nb_samples; i++)
        dst[i] = av_clip_uint8((((src[i] - 128) * volume + 128) >> 8) + 128);
}

static inline void scale_samples_s16(uint8_t *dst, const uint8_t *src,
                                     int nb_samples, int volume)
{
    int i;
    int16_t *smp_dst = (int16_t *)dst;
    const int16_t *smp_src = (const int16_t *)src;
    for (i = 0; i < nb_samples; i++)
        smp_dst[i] = av_clip_int16(((int64_t)smp_src[i] * volume + 128) >> 8);
}

static inline void scale_samples_s16_small(uint8_t *dst, const uint8_t *src,
                                           int nb_samples, int volume)
{
    int i;
    int16_t *smp_dst = (int16_t *)dst;
    const int16_t *smp_src = (const int16_t *)src;
    for (i = 0; i < nb_samples; i++)
        smp_dst[i] = av_clip_int16((smp_src[i] * volume + 128) >> 8);
}

static inline void scale_samples_s32(uint8_t *dst, const uint8_t *src,
                                     int nb_samples, int volume)
{
    int i;
    int32_t *smp_dst = (int32_t *)dst;
    const int32_t *smp_src = (const int32_t *)src;
    for (i = 0; i < nb_samples; i++)
        smp_dst[i] = av_clipl_int32((((int64_t)smp_src[i] * volume + 128) >> 8));
}

static av_cold void scaler_init(VolumeContext *vol)
{
    int32_t volume_i = (int32_t)(vol->volume * 256 + 0.5);
    vol->samples_align = 1;

    switch (av_get_packed_sample_fmt(vol->sample_fmt)) {
    case AV_SAMPLE_FMT_U8:
        if (volume_i < 0x1000000)
            vol->scale_samples = scale_samples_u8_small;
        else
            vol->scale_samples = scale_samples_u8;
        break;
    case AV_SAMPLE_FMT_S16:
        if (volume_i < 0x10000)
            vol->scale_samples = scale_samples_s16_small;
        else
            vol->scale_samples = scale_samples_s16;
        break;
    case AV_SAMPLE_FMT_S32:
        vol->scale_samples = scale_samples_s32;
        break;
    case AV_SAMPLE_FMT_FLT:
        vol->samples_align = 4;
        break;
    case AV_SAMPLE_FMT_DBL:
        vol->samples_align = 8;
        break;
    }
}

void volume_set(VolumeContext *vol, double volume)
{
    /* Should not fade in first frame, cause there is no src volume. */
    vol->volume_last = vol->volume_last > 0 ? vol->volume : volume;

    vol->volume = volume;

    scaler_init(vol);
}

void volume_scale(VolumeContext *vol, AVFrame *frame)
{
    int planar, planes, plane_size, p;
    planar = av_sample_fmt_is_planar(frame->format);
    planes = planar ? frame->ch_layout.nb_channels : 1;
    plane_size = frame->nb_samples * (planar ? 1 : frame->ch_layout.nb_channels);

    if (vol->volume_last < 0)
        vol->volume_last = vol->volume; /* If volume not set after init, skip fade in first frame. */

    if (frame->format == AV_SAMPLE_FMT_S16 ||
        frame->format == AV_SAMPLE_FMT_S16P) {
        int32_t vol_isrc = (int32_t)(vol->volume_last * 256 + 0.5);
        int32_t volume_i = (int32_t)(vol->volume * 256 + 0.5);
        if (volume_i != vol_isrc) {
            for (p = 0; p < planes; p++) {
                vol->fade_samples((int16_t *)frame->extended_data[p],
                                  (int16_t *)frame->extended_data[p],
                                  frame->nb_samples, planar ? 1 : frame->ch_layout.nb_channels,
                                  volume_i, vol_isrc);
            }
        } else {
            for (p = 0; p < planes; p++) {
                vol->scale_samples(frame->extended_data[p],
                                   frame->extended_data[p],
                                   plane_size, volume_i);
            }
        }
        vol->volume_last = vol->volume;
    } else if (frame->format == AV_SAMPLE_FMT_FLT ||
                       frame->format == AV_SAMPLE_FMT_FLTP) {
        for (p = 0; p < planes; p++) {
            vol->fdsp->vector_fmul_scalar((float *)frame->extended_data[p],
                                          (float *)frame->extended_data[p],
                                          vol->volume, plane_size);
        }
    } else {
        for (p = 0; p < planes; p++) {
            vol->fdsp->vector_dmul_scalar((double *)frame->extended_data[p],
                                          (double *)frame->extended_data[p],
                                          vol->volume, plane_size);
        }
    }
}

int volume_init(VolumeContext *vol, enum AVSampleFormat sample_fmt)
{
    vol->sample_fmt = sample_fmt;

    vol->volume_last = -1.0f;
    vol->volume = 1.0f;

    vol->fdsp = avpriv_float_dsp_alloc(0);
    if (!vol->fdsp)
        return AVERROR(ENOMEM);

    scaler_init(vol);
    vol->fade_samples = fade_samples_s16_small;

    return 0;
}

void volume_uninit(VolumeContext *vol)
{
    av_freep(&vol->fdsp);
}