/*
 * audio resampling with hifi4 optimization
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
 * audio resampling with hifi4 optimization
 */

#include "libavutil/log.h"
#include "libavutil/avassert.h"
#include "swresample_internal.h"
#include "resample.h"

#include <smf_process_resample.h>

typedef struct HIFI4ResampleContext {
    smf_resample_open_param_t param;
    void                      *resamples[SWR_CH_MAX];
} HIFI4ResampleContext;

static struct ResampleContext *create(struct ResampleContext *c, int out_rate, int in_rate, int filter_size, int phase_shift, int linear,
        double cutoff, enum AVSampleFormat format, enum SwrFilterType filter_type, double kaiser_beta, double precision, int cheby, int exact_rational)
{
    HIFI4ResampleContext *context;

    context = (HIFI4ResampleContext *)av_mallocz(sizeof(HIFI4ResampleContext));
    if (!context)
        return NULL;

    smf_resample_register();

    context->param.rate        = in_rate;
    context->param.target_rate = out_rate;

    switch (format) {
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            context->param.target_sampleBits = 16;
            context->param.sampleBits = 16;
            break;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            context->param.target_sampleBits = 32;
            context->param.sampleBits = 32;
            break;
        default:
            goto error;
    }

    return (ResampleContext *)context;

error:
    if (context)
        av_free(context);

    return NULL;
}

static void destroy(struct ResampleContext **c)
{
    HIFI4ResampleContext *context = (HIFI4ResampleContext *)*c;
    int i;

    if (context) {
       for (i = 0; i < SWR_CH_MAX; i++) {
            if (context->resamples[i]) {
                smf_close(context->resamples[i]);
                smf_destroy(context->resamples[i]);
            }
        }

        av_free(context);
    }
}

static int flush(struct SwrContext *s)
{
    return 0;
}

static int process(
        struct ResampleContext * c, AudioData *dst, int dst_size,
        AudioData *src, int src_size, int *consumed)
{
    HIFI4ResampleContext *context = (HIFI4ResampleContext *) c;
    smf_frame_t input, output;
    int i, chs, ret = 0;

    if (!context) {
        av_log(0, AV_LOG_ERROR, "%s resample error.\n", __func__);
        return AVERROR(EINVAL);
    }

    av_assert0(src->planar == dst->planar);
    av_assert0(src->ch_count == dst->ch_count);

    chs = src->planar ? src->ch_count : 1;
    for (i = 0; i < chs; i++) {

        /* create resampler handle if NULL */
        if (!context->resamples[i]) {
            context->resamples[i] = smf_create_processer("resamp");
            if (!context->resamples[i]) {
                av_log(0, AV_LOG_ERROR, "%s create resamp failed. \n", __func__);
                return AVERROR(ENOMEM) ;
            }

            context->param.channels        = src->planar ? 1 : src->ch_count;
            context->param.target_channels = dst->planar ? 1 : dst->ch_count;

            if (!smf_open(context->resamples[i], &context->param)) {
                av_log(0, AV_LOG_ERROR, "%s open resamp failed. \n", __func__);
                return AVERROR(ENOSYS);
            }
        }

        /* prepare resample parameters */
        memset(&output, 0, sizeof(output));
        memset(&input, 0, sizeof(input));

        output.buff = dst->ch[i];
        output.max  = dst_size * dst->bps;
        input.buff  = src->ch[i];
        input.size  = src_size * src->bps;
        input.max   = src_size * src->bps;

        /* do resample */
        while (1) {
            if (!smf_process(context->resamples[i], &input, &output)) {
                av_log(0, AV_LOG_ERROR, "%s do resample failed.\n", __func__);
                ret = AVERROR(ENOSYS);
                break;
            }

            if (!input.size)
                break;
        }
    }

    *consumed = src_size - input.size / src->bps;
    return ret < 0 ? ret : output.size / dst->bps;
}

static int64_t get_delay(struct SwrContext *s, int64_t base)
{
    return 0;
}

static int invert_initial_buffer(struct ResampleContext *c, AudioData *dst, const AudioData *src,
                                 int in_count, int *out_idx, int *out_sz)
{
    return 0;
}

static int64_t get_out_samples(struct SwrContext *s, int in_samples)
{
    int64_t num = in_samples + 2LL;
    num = av_rescale_rnd(in_samples, s->out_sample_rate, (int64_t)s->in_sample_rate, AV_ROUND_UP) + 2;
    return num;
}

struct Resampler const swri_hifi4_resampler = {
    create, destroy, process, flush, NULL /* set_compensation */, get_delay,
    invert_initial_buffer, get_out_samples
};
