/*
 * Audio resampling with SRC
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
 * Audio resampling with SRC(Secret Rabbit Code, libsamplerate)
 */

#include "libavutil/log.h"
#include "libavutil/avassert.h"
#include "swresample_internal.h"

#include <samplerate.h>

typedef struct SrcResamplerContext {
    int converter;
    int channels;
    double ratio;
    SRC_STATE *src_state;
} SrcResamplerContext;

static struct ResampleContext *create(struct ResampleContext *c, int out_rate,
                                      int in_rate, int filter_size,
                                      int phase_shift, int linear,
                                      double cutoff, enum AVSampleFormat format,
                                      enum SwrFilterType filter_type,
                                      double kaiser_beta, double precision,
                                      int cheby, int exact_rational)
{
    SrcResamplerContext *src_ctx;
    int converter;

    switch (filter_type) {
        case SWR_FILTER_TYPE_SINC_BEST:
            converter = SRC_SINC_BEST_QUALITY; break;
        case SWR_FILTER_TYPE_SINC_MEDIUM:
            converter = SRC_SINC_MEDIUM_QUALITY; break;
        case SWR_FILTER_TYPE_SINC_FAST:
            converter = SRC_SINC_FASTEST; break;
        case SWR_FILTER_TYPE_ZOH:
            converter = SRC_ZERO_ORDER_HOLD; break;
        case SWR_FILTER_TYPE_LINEAR:
            converter = SRC_LINEAR; break;
        default:
            converter = SRC_DEFAULT_CONVERTER;
            break;
    }

    src_ctx = av_mallocz(sizeof(SrcResamplerContext));
    if (!src_ctx)
        goto error;

    src_ctx->converter = converter;
    src_ctx->ratio     = out_rate / (double)in_rate;

    return (struct ResampleContext *)src_ctx;
error:
    return NULL;
}

static void destroy(struct ResampleContext **c)
{
    SrcResamplerContext *src_ctx = (SrcResamplerContext*)*c;

    if (src_ctx && src_ctx->src_state)
        src_delete(src_ctx->src_state);

    av_freep(&src_ctx);
}

static int flush(struct SwrContext *s)
{
    SrcResamplerContext *src_ctx = (SrcResamplerContext *)s->resample;
    SRC_STATE *src_state = src_ctx->src_state;
    SRC_DATA src_data;
    int error;

    src_data.data_in       = NULL;
    src_data.data_out      = NULL;
    src_data.input_frames  = 0;
    src_data.output_frames = 0;
    src_data.end_of_input  = 1;
    src_data.src_ratio     = src_ctx->ratio;

    error = src_process(src_state, &src_data);
    if (error)
        av_log(NULL, AV_LOG_ERROR, "SRC flush, %s\n", src_strerror(error));

    return AVERROR(error);
}

static int process(struct ResampleContext *c, AudioData *dst,
                   int dst_size, AudioData *src, int src_size, int *consumed)
{
    SrcResamplerContext *src_ctx = (SrcResamplerContext*)c;
    int channels = src->ch_count;
    SRC_DATA src_data;
    int error;

    if (!src_ctx->src_state) {
        src_ctx->src_state = src_new(src_ctx->converter, channels, &error);
        if (!src_ctx->src_state) {
            av_log(NULL, AV_LOG_ERROR, "Create SRC, converter %d, %s\n",
                                       src_ctx->converter, src_strerror(error));
            return AVERROR(error);
        }
    }

    src_data.data_in       = (const float *)src->data;
    src_data.data_out      = (float *)dst->data;
    src_data.input_frames  = src_size * channels;
    src_data.output_frames = dst_size * channels;
    src_data.src_ratio     = src_ctx->ratio;
    src_data.end_of_input  = 0;

    error = src_process(src_ctx->src_state, &src_data);
    if (error) {
        av_log(NULL, AV_LOG_ERROR, "SRC process, %s\n", src_strerror(error));
        return AVERROR(error);
    }

    *consumed = src_data.input_frames_used / channels;
    return src_data.output_frames_gen / channels;
}

static int64_t get_delay(struct SwrContext *s, int64_t base)
{
    return 0;
}

static int invert_initial_buffer(struct ResampleContext *c,
                                 AudioData *dst, const AudioData *src,
                                 int in_count, int *out_idx, int *out_sz)
{
    return 0;
}

static int64_t get_out_samples(struct SwrContext *s, int in_samples)
{
    double out_samples =
        (double)s->out_sample_rate / s->in_sample_rate * in_samples;

    return (int64_t)out_samples;
}

struct Resampler const swri_src_resampler = {
    create,
    destroy,
    process,
    flush,
    NULL /* set_compensation */,
    get_delay,
    invert_initial_buffer,
    get_out_samples
};
