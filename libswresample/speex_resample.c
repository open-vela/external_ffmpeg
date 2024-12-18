/*
 * Audio resampling with speexdsp
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
 * Audio resampling with speexdsp
 */

#include "libavutil/log.h"
#include "libavutil/avassert.h"
#include "swresample_internal.h"
#include "resample.h"

#include "speex_resampler.h"

typedef struct SpeexResampleContext {
    SpeexResamplerState *resampler;
    int in_rate;
    int out_rate;
} SpeexResampleContext;

static struct ResampleContext *create(struct ResampleContext *c, int out_rate, int in_rate, int filter_size, int phase_shift, int linear,
        double cutoff, enum AVSampleFormat format, enum SwrFilterType filter_type, double kaiser_beta, double precision, int cheby, int exact_rational)
{
    SpeexResampleContext *context;

    context = (SpeexResampleContext *)av_mallocz(sizeof(SpeexResampleContext));
    if (!context)
        return NULL;

    context->in_rate = in_rate;
    context->out_rate = out_rate;

    return (struct ResampleContext *)context;
}

static void destroy(struct ResampleContext **c)
{
    SpeexResampleContext *context;

    context = (SpeexResampleContext *)*c;
    if (!context)
        return;

    if (context->resampler)
        speex_resampler_destroy(context->resampler);

    av_freep(&context);
    *c = NULL;
}

static int flush(struct SwrContext *s)
{
    SpeexResampleContext *context;
    int err;

    context = (SpeexResampleContext *)s->resample;
    if (!context || !context->resampler)
        return AVERROR(EINVAL);

    err = speex_resampler_reset_mem(context->resampler);
    if (err != RESAMPLER_ERR_SUCCESS)
        av_log(NULL, AV_LOG_ERROR, "speex flush resampler fail: %s\n",
               speex_resampler_strerror(err));

    return AVERROR(err);
}

static int process(
        struct ResampleContext *c, AudioData *dst, int dst_size,
        AudioData *src, int src_size, int *consumed)
{
    SpeexResampleContext *context;
    const spx_int16_t *in_buffer;
    spx_int16_t *out_buffer;
    spx_uint32_t in_samples;
    spx_uint32_t out_samples;
    int err;
    int ch;

    context = (SpeexResampleContext *)c;
    if (!context) {
        av_log(NULL, AV_LOG_ERROR, "speex resample context error.\n");
        return AVERROR(EINVAL);
    }

    if (src->planar != dst->planar) {
        av_log(NULL, AV_LOG_ERROR, "speex src and dst planar formats do not match.\n");
        return AVERROR(EINVAL);
    }

    if (src->ch_count != dst->ch_count) {
        av_log(NULL, AV_LOG_ERROR, "speex src and dst channel counts do not match.\n");
        return AVERROR(EINVAL);
    }

    if (!context->resampler) {
        context->resampler = speex_resampler_init(src->ch_count, context->in_rate, context->out_rate,
                                                  SPEEX_RESAMPLER_QUALITY_MIN, &err);
        if (err != RESAMPLER_ERR_SUCCESS) {
            av_log(NULL, AV_LOG_ERROR, "speex initialize resampler fail: %s\n",
                   speex_resampler_strerror(err));
            return AVERROR(err);
        }
    }

    in_samples = src_size / src->bps / src->ch_count;
    out_samples = dst_size / dst->bps / src->ch_count;

    if (src->planar) {
        for (ch = 0; ch < src->ch_count; ch++) {
            in_buffer = (const spx_int16_t *)src->ch[ch];
            out_buffer = (spx_int16_t *)dst->ch[ch];

            err = speex_resampler_process_int(context->resampler, ch, in_buffer,
                                              &in_samples, out_buffer, &out_samples);
            if (err != RESAMPLER_ERR_SUCCESS) {
                av_log(NULL, AV_LOG_ERROR, "speex resample planar data fail %s\n",
                       speex_resampler_strerror(err));
                return AVERROR(err);
            }
        }
    } else {
        in_buffer = (const spx_int16_t *)src->data;
        out_buffer = (spx_int16_t *)dst->data;

        err = speex_resampler_process_interleaved_int(context->resampler, in_buffer,
                                                      &in_samples, out_buffer, &out_samples);
        if (err != RESAMPLER_ERR_SUCCESS) {
            av_log(NULL, AV_LOG_ERROR, "speex resample interleaved data fail %s\n",
                   speex_resampler_strerror(err));
            return AVERROR(err);
        }
    }

    *consumed = in_samples;
    return out_samples;
}

static int64_t get_delay(struct SwrContext *s, int64_t base)
{
    SpeexResampleContext *context;

    context = (SpeexResampleContext *)s->resample;
    if (!context || !context->resampler)
        return AVERROR(EINVAL);

    return speex_resampler_get_input_latency(context->resampler) * base;
}

struct Resampler const swri_speex_resampler = {
    create,
    destroy,
    process,
    flush,
    NULL,
    get_delay,
    NULL,
    NULL
};