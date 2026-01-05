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
#include "libavutil/samplefmt.h"
#include <math.h>
#include <limits.h>
#include "volume.h"

#define CONVERT_PACKED(src_type, dst_type, src_enum, dst_enum, convert_expr) \
    case src_enum: \
        if (dst_fmt == dst_enum) { \
            const src_type *s = (const src_type *)src; \
            dst_type *d = (dst_type *)dst; \
            int total = nb_samples * chs; \
            for (int i = 0; i < total; i++) d[i] = convert_expr; \
            return 0; \
        } \
        break

#define CONVERT_PLANAR(src_type, dst_type, src_enum, dst_enum, convert_expr) \
    case src_enum: \
        if (dst_fmt == dst_enum) { \
            const src_type *const *s = (const src_type *const *)src; \
            dst_type **d = (dst_type **)dst; \
            for (int c = 0; c < chs; c++) { \
                for (int i = 0; i < nb_samples; i++) d[c][i] = convert_expr; \
            } \
            return 0; \
        } \
        break

static int bit_convert(enum AVSampleFormat src_fmt, enum AVSampleFormat dst_fmt,
                       const void *src, void *dst, int nb_samples, int chs) {
    /* Check integer multiplication overflow */
    if ((long long)nb_samples * chs > INT_MAX) {
        av_log(NULL, AV_LOG_ERROR, "Total samples overflow\n");
        return AVERROR(ERANGE);
    }

    switch (src_fmt) {
        CONVERT_PACKED(uint8_t, float, AV_SAMPLE_FMT_U8,  AV_SAMPLE_FMT_FLT,  (s[i] - 128.0f) / 128.0f);
        CONVERT_PLANAR(uint8_t, float, AV_SAMPLE_FMT_U8P, AV_SAMPLE_FMT_FLTP, (s[c][i] - 128.0f) / 128.0f);
        CONVERT_PACKED(int16_t, float, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_FLT,  s[i] / 32767.0f);
        CONVERT_PLANAR(int16_t, float, AV_SAMPLE_FMT_S16P,AV_SAMPLE_FMT_FLTP, s[c][i] / 32767.0f);
        CONVERT_PACKED(int32_t, float, AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_FLT,  s[i] / 2147483647.0f);
        CONVERT_PLANAR(int32_t, float, AV_SAMPLE_FMT_S32P,AV_SAMPLE_FMT_FLTP, s[c][i] / 2147483647.0f);
        default: break;
    }

    switch (src_fmt) {
        CONVERT_PACKED(float, int16_t, AV_SAMPLE_FMT_FLT,  AV_SAMPLE_FMT_S16,  av_clip_int16(lrintf(s[i] * 32767.0f)));
        CONVERT_PLANAR(float, int16_t, AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_S16P, av_clip_int16(lrintf(s[c][i] * 32767.0f)));
        default: break;
    }

    av_log(NULL, AV_LOG_ERROR, "Unsupported convert: %s → %s\n",
           av_get_sample_fmt_name(src_fmt), av_get_sample_fmt_name(dst_fmt));
    return AVERROR(EINVAL);
}

static inline void fade_samples_u8_small(uint8_t *dst, const uint8_t *src,
                                         int nb_samples, int chs, int16_t dst_volume, int16_t src_volume)
{
    int i, j, k = 0;
    int32_t step;

    step = ((int32_t)(dst_volume - src_volume) * (1 << 15)) / nb_samples;
    for (i = 0; i < nb_samples; i++) {
        for (j = 0; j < chs; j++, k++) {
            dst[k] = av_clip_uint8(((src[k] * (src_volume + (step * i >> 15))) >> 8));
        }
    }
}

static inline void fade_samples_s16_small(uint8_t *dst, const uint8_t *src,
                                          int nb_samples, int chs, int16_t dst_volume, int16_t src_volume)
{
    int i, j, k = 0;
    int32_t step;
    int16_t *smp_dst = (int16_t *)dst;
    const int16_t *smp_src = (const int16_t *)src;

    step = ((int32_t)(dst_volume - src_volume) * (1 << 15)) / nb_samples;
    for (i = 0; i < nb_samples; i++) {
        for (j = 0; j < chs; j++, k++) {
            smp_dst[k] = av_clip_int16((smp_src[k] * (src_volume + (step * i >> 15))) >> 8);
        }
    }
}

static inline void fade_samples_s32_small(uint8_t *dst, const uint8_t *src,
                                          int nb_samples, int chs, int16_t dst_volume, int16_t src_volume)
{
    int i, j, k = 0;
    int64_t step;
    int32_t *smp_dst = (int32_t *)dst;
    const int32_t *smp_src = (const int32_t *)src;

    step = ((int64_t)(dst_volume - src_volume) * (1LL << 31)) / nb_samples;
    for (i = 0; i < nb_samples; i++) {
        for (j = 0; j < chs; j++, k++) {
            smp_dst[k] = av_clipl_int32(((int64_t)smp_src[k] * (src_volume + (step * i >> 31))) >> 8);
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
    /* Use standard rounding function */
    int32_t volume_i = (int32_t)lround(vol->volume * 256);
    vol->samples_align = 1;

    /* use the processing format (mid_fmt) so pointers stay valid after down-convert */
    switch (av_get_packed_sample_fmt(vol->mid_fmt)) {
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

static av_cold void fader_init(VolumeContext *vol)
{
    /* Keep fade function pointers in sync with the processing format. */
    switch (av_get_packed_sample_fmt(vol->mid_fmt)) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
        vol->fade_samples = fade_samples_u8_small;
        break;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
        vol->fade_samples = fade_samples_s16_small;
        break;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        vol->fade_samples = fade_samples_s32_small;
        break;
    default:
        vol->fade_samples = NULL;
        break;
    }
}

static bool is_fixed(enum AVSampleFormat fmt)
{
    switch (fmt) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
        return true;
    default:
        return false;
    }
}

static bool is_float(enum AVSampleFormat fmt)
{
    switch (fmt) {
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
        return true;
    default:
        return false;
    }
}

static enum AVSampleFormat pick_mid_fmt(enum AVSampleFormat src, enum PrecisionType precision)
{
    int planar = av_sample_fmt_is_planar(src);

    if (precision == PRECISION_FLOAT && (
        src == AV_SAMPLE_FMT_U8  || src == AV_SAMPLE_FMT_U8P  ||
        src == AV_SAMPLE_FMT_S16 || src == AV_SAMPLE_FMT_S16P ||
        src == AV_SAMPLE_FMT_S32 || src == AV_SAMPLE_FMT_S32P))
        return planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;

    if (precision == PRECISION_FIXED && (
        src == AV_SAMPLE_FMT_FLT  || src == AV_SAMPLE_FMT_FLTP))
        return planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;

    return src;
}

static AVFrame *get_conv_frame(VolumeContext *vol, AVFrame *src, enum AVSampleFormat fmt)
{
    int ret;

    if (!vol->conv_frame)
        return NULL;

    if (vol->conv_frame->format != fmt ||
        vol->conv_frame->nb_samples != src->nb_samples) {
        av_frame_unref(vol->conv_frame);

        vol->conv_frame->format = fmt;
        vol->conv_frame->nb_samples = src->nb_samples;
        vol->conv_frame->sample_rate = src->sample_rate;
        vol->conv_frame->ch_layout = src->ch_layout;

        ret = av_frame_get_buffer(vol->conv_frame, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to get conv frame buffer\n");
            return NULL;
        }
    }

    return vol->conv_frame;
}

void volume_set(VolumeContext *vol, double volume)
{
    /* Should not fade in first frame, cause there is no src volume. */
    vol->volume_last = vol->volume_last > 0 ? vol->volume : volume;

    vol->volume = volume;

    scaler_init(vol);
    fader_init(vol);
}

void volume_scale(VolumeContext *vol, AVFrame *frame)
{
    int planar, planes, plane_size, p, need_convert, ret;
    AVFrame *proc_frame = frame;

    if (!vol || !frame)
        return;

    planar = av_sample_fmt_is_planar(frame->format);
    planes = planar ? frame->ch_layout.nb_channels : 1;
    plane_size = frame->nb_samples * (planar ? 1 : frame->ch_layout.nb_channels);
    need_convert = (vol->mid_fmt != frame->format);

    if (vol->volume_last < 0)
        vol->volume_last = vol->volume; /* If volume not set after init, skip fade in first frame. */

    /* convert to intermediate format if needed */
    if (need_convert) {
        proc_frame = get_conv_frame(vol, frame, vol->mid_fmt);
        if (!proc_frame) {
            av_log(NULL, AV_LOG_ERROR, "get_conv_frame failed\n");
            return;
        }

        ret = bit_convert(frame->format, vol->mid_fmt,
                         planar ? (const void *)frame->extended_data : frame->data[0],
                         av_sample_fmt_is_planar(vol->mid_fmt) ? (void *)proc_frame->extended_data : proc_frame->data[0],
                         frame->nb_samples, frame->ch_layout.nb_channels);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Convert to mid format failed\n");
            return;
        }
    }

    /* apply volume scaling / fading */
    if (is_fixed(proc_frame->format)) {
        int32_t vol_isrc = (int32_t)lround(vol->volume_last * 256);
        int32_t volume_i = (int32_t)lround(vol->volume * 256);
        if (volume_i != vol_isrc) {
            for (p = 0; p < planes; p++) {
                vol->fade_samples(proc_frame->extended_data[p],
                                  proc_frame->extended_data[p],
                                  proc_frame->nb_samples, planar ? 1 : proc_frame->ch_layout.nb_channels,
                                  volume_i, vol_isrc);
            }
        } else {
            for (p = 0; p < planes; p++) {
                vol->scale_samples(proc_frame->extended_data[p],
                                   proc_frame->extended_data[p],
                                   plane_size, volume_i);
            }
        }
        vol->volume_last = vol->volume;
    } else if (is_float(proc_frame->format)) {
        for (p = 0; p < planes; p++) {
            vol->fdsp->vector_fmul_scalar((float *)proc_frame->extended_data[p],
                                          (float *)proc_frame->extended_data[p],
                                          vol->volume, plane_size);
        }
    } else {
        for (p = 0; p < planes; p++) {
            vol->fdsp->vector_dmul_scalar((double *)proc_frame->extended_data[p],
                                          (double *)proc_frame->extended_data[p],
                                          vol->volume, plane_size);
        }
    }

    /* convert back to original format if needed */
    if (need_convert) {
        ret = bit_convert(vol->mid_fmt, frame->format,
                         av_sample_fmt_is_planar(vol->mid_fmt) ? (const void *)proc_frame->extended_data : proc_frame->data[0],
                         planar ? (void *)frame->extended_data : frame->data[0],
                         frame->nb_samples, frame->ch_layout.nb_channels);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Convert to original format failed\n");
            return;
        }
    }
}

int volume_init(VolumeContext *vol, enum AVSampleFormat sample_fmt, enum PrecisionType precision)
{
    if (!vol) {
        av_log(NULL, AV_LOG_ERROR, "VolumeContext is NULL\n");
        return AVERROR(EINVAL);
    }

    vol->sample_fmt = sample_fmt;
    vol->precision = precision;
    vol->volume_last = -1.0f;
    vol->volume = 1.0f;

    av_frame_free(&vol->conv_frame);
    vol->conv_frame = av_frame_alloc();
    if (!vol->conv_frame)
        return AVERROR(ENOMEM);

    vol->mid_fmt = pick_mid_fmt(sample_fmt, precision);

    if (vol->fdsp)
        av_freep(&vol->fdsp);

    vol->fdsp = avpriv_float_dsp_alloc(0);
    if (!vol->fdsp) {
        av_frame_free(&vol->conv_frame);
        return AVERROR(ENOMEM);
    }

    scaler_init(vol);
    fader_init(vol);
    return 0;
}

void volume_uninit(VolumeContext *vol)
{
    av_freep(&vol->fdsp);
    if (vol->conv_frame)
        av_frame_free(&vol->conv_frame);
}

int volume_parse_index_db(const char *str, int *index, double *value)
{
    const char *p;
    char *end;
    long idx;
    int ret;

    if (!str || !index || !value)
        return AVERROR(EINVAL);

    p = str;
    while (av_isspace(*p))
        p++;

    *index = -1;

    idx = strtol(p, &end, 0);

    if (end != p && av_isspace(*end)) {
        if (idx < -1 || idx > INT_MAX)
            return AVERROR(EINVAL);

        *index = (int)idx;

        p = end;
        while (av_isspace(*p))
            p++;
    }

    ret = av_expr_parse_and_eval(value, p,
                                 NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, 0, NULL);
    return ret;
}