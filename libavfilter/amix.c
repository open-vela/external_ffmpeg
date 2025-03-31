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
 * mix audio module
 */

#include "amix.h"
#include "aresample.h"
#include "audio.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "formats.h"
#include "framepool.h"

#include "libavutil/audio_fifo.h"
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/eval.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"

#include <sys/queue.h>

#define INPUT_ON       1    /**< input is active */
#define INPUT_EOF      2    /**< input has reached EOF (may still be active) */

typedef struct AMixInput {
    AMixContext *parent;        /**< amix context */
    float scale;                /**< scale for each input */
    uint8_t state;              /**< current state of each input */
    int64_t duration;           /**< output duration of the input */
    AVFilterLink *link;         /**< link for data resource */
    AResampleContext *resample; /**< resampler context */
    AVAudioFifo *fifo;          /**< fifo to store resampled data before mix */
    TAILQ_ENTRY(AMixInput)
    entries;
} AMixInput;

struct AMixContext {
    int nb_inputs;          /**< number of inputs */
    AVFilterLink *out;      /**< not a real link, to store formats info and provide framepool.*/
    TAILQ_HEAD(, AMixInput)
    inputs;                 /**< inputs for mix */
};

/**
 * Update the scaling factors to apply to each input during mixing.
 *
 * This balances the full volume range between active inputs and handles
 * volume transitions when EOF is encountered on an input but mixing continues
 * with the remaining inputs.
 */
static void calculate_scales(AMixContext *s)
{
    int activate_inputs = 0;
    AMixInput *input;
    int i;

    if (TAILQ_EMPTY(&s->inputs))
        return;

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (input->state & INPUT_ON)
            activate_inputs++;
    }

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (input->state & INPUT_ON) {
            input->scale = 1.0 / activate_inputs;
        }
    }
}

static int get_output_samples(AMixContext *s)
{
    int ns, nb_samples = INT_MAX;
    AMixInput *input;

    if (TAILQ_EMPTY(&s->inputs))
        return 0;

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (input->state & INPUT_ON) {
            if (!input->fifo)
                ns = ff_inlink_queued_samples(input->link);
            else
                ns = av_audio_fifo_size(input->fifo);

            if (ns != 0)
                nb_samples = FFMIN(nb_samples, ns);
        }
    }

    return nb_samples;
}

static void vector_fmac_scalar_c(int16_t *dst, const int16_t *src, int16_t mul, int len)
{
    int32_t accu;
    int i;

    for (i = 0; i < len; i++) {
        accu = (int32_t)src[i] * mul;
        dst[i] = av_clip_int16(dst[i] + ((accu + 0x4000) >> 15));
    }
}

static AMixInput *amix_input_alloc(AMixContext *s, AVFilterLink *link)
{
    FilterLinkInternal *li;
    AMixInput *input;
    int ret;

    input = av_mallocz(sizeof(*input));
    if (!input)
        return NULL;

    input->parent = s;

    if (link->sample_rate != s->out->sample_rate || link->format != s->out->format ||
        av_channel_layout_compare(&link->ch_layout, &s->out->ch_layout)) {

        input->fifo = av_audio_fifo_alloc(s->out->format, s->out->ch_layout.nb_channels, 1024);
        if (!input->fifo)
            goto err;

        input->resample = av_malloc(sizeof(*input->resample));
        if (!input->resample)
            goto err;
        ff_resample_init(input->resample);
    }
    input->link = link;

    TAILQ_INSERT_TAIL(&s->inputs, input, entries);
    s->nb_inputs++;

    return input;

err:
    if (input->fifo)
        av_audio_fifo_free(input->fifo);
    av_freep(&input);
    return NULL;
}

static void amix_input_free(AMixInput *in)
{
    AMixContext *s;
    int i;

    if (!in)
        return;

    s = in->parent;

    TAILQ_REMOVE(&s->inputs, in, entries);

    if (in->resample && in->fifo) {
        av_audio_fifo_free(in->fifo);
        in->fifo = NULL;
        ff_resample_uninit(in->resample);
        av_freep(&in->resample);
    }

    av_free(in);
    s->nb_inputs--;
}

static AMixInput *amix_find_input(AMixContext *s, AVFilterLink *link)
{
    AMixInput *input;

    if (TAILQ_EMPTY(&s->inputs))
        return NULL;

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (input->link == link)
            return input;
    }

    return NULL;
}

/**
 * Set input's state according to current queued_samples in link and link status.
 *
 * case1: queued_samples > 0, and link status is not AVERROR_EOF, input->state = INPUT_ON;
 *
 * case2: queued_samples > 0, and link status is AVERROR_EOF, input->state |= INPUT_EOF;
 *
 * case3: queued_samples == 0, and link status is AVERROR_EOF, input->state &= ~INPUT_ON;
 *
 * case4: queued_samples == 0, and link status is not AVERROR_EOF, input->state &= ~INPUT_ON;
 */
static void input_sync_state(AMixInput *input)
{
    AMixContext *s = input->parent;
    AVFilterLink *link = input->link;

    if (input->state & INPUT_EOF && ff_amix_input_empty(s, link)) {
        input->state &= ~INPUT_ON;
        return;
    }

    if (!ff_amix_input_empty(s, link)){
        if (ff_outlink_get_status(link) != AVERROR_EOF)
            input->state = INPUT_ON;
        else
            input->state |= INPUT_EOF;
    }
}

AMixContext *ff_amix_alloc(int sample_rate, int format, int channels)
{
    FilterLinkInternal *li;
    AMixContext *s;
    int i;

    s = av_mallocz(sizeof(*s));
    if (!s)
        return NULL;

    li = av_mallocz(sizeof(*li));
    if (!li) {
        av_free(s);
        return NULL;
    }
    s->out = &li->l.pub;

    av_channel_layout_default(&s->out->ch_layout, channels);
    s->out->format = format;
    s->out->sample_rate = sample_rate;

    TAILQ_INIT(&s->inputs);

    return s;
}

void ff_amix_free(AMixContext *s)
{
    FilterLinkInternal *li;
    AMixInput *input;
    int i;

    if (!s)
        return;

    while (input = TAILQ_FIRST(&s->inputs))
        amix_input_free(input);

    li = ff_link_internal(s->out);
    if (li->frame_pool)
        ff_frame_pool_uninit(&li->frame_pool);
    av_channel_layout_uninit(&s->out->ch_layout);
    av_freep(&s->out);
    av_free(s);
}

int ff_amix_input_empty(AMixContext *s, AVFilterLink *link)
{
    AMixInput *input;

    if (!s || !link)
        return AVERROR(EINVAL);

    input = amix_find_input(s, link);

    return !input || !input->link || (input->fifo ? av_audio_fifo_size(input->fifo) == 0 : !ff_inlink_check_available_samples(input->link, 1));
}

int ff_amix_input_want(AMixContext *s, AVFilterLink *link)
{
    AMixInput *input;

    if (!s || !link)
        return AVERROR(EINVAL);

    input = amix_find_input(s, link);

    return !input || (!(input->state & INPUT_EOF) &&
           (!input->link || (av_rescale_q(input->fifo ? av_audio_fifo_size(input->fifo) == 0 : ff_inlink_queued_samples(input->link),
           av_make_q(1, input->link->sample_rate), AV_TIME_BASE_Q) <= input->duration)));
}

/* where param "link" is a real link*/
int ff_amix_input_write(AMixContext *s, AVFilterLink *link)
{
    AVFrame *frame, *rframe;
    AMixInput *input;
    int ret;

    if (!s || !link)
        return AVERROR(EINVAL);

    input = amix_find_input(s, link);
    if (!input) {
        input = amix_input_alloc(s, link);
        if (!input)
            return AVERROR(ENOMEM);
    }

    if (ff_inlink_check_available_frame(link)) {
        if (input->resample) {
            ret = ff_inlink_consume_frame(link, &frame);
            if (ret < 0)
                return ret;

            ret = ff_resample_frame(input->resample, s->out, frame, &rframe);
            if (ret <= 0)
                return ret;

            ret = av_audio_fifo_write(input->fifo, (void **)rframe->extended_data, rframe->nb_samples);
            if (ret < 0) {
                av_frame_free(&rframe);
                return ret;
            }
            av_frame_free(&rframe);
            av_frame_free(&frame);
        }
    }

    input_sync_state(input);

    return 0;
}

int ff_amix_read(AMixContext *s, AVFrame **oframe)
{
    int planes, plane_size, p, planar;
    AVFrame *out_buf, *in_buf;
    int  i, ret, nb_samples;
    AMixInput *input;
    int64_t pts;

    if (!s)
        return AVERROR(EINVAL);

    if (TAILQ_EMPTY(&s->inputs))
        return AVERROR(EINVAL);

    nb_samples = get_output_samples(s);
    if (nb_samples == INT_MAX)
        return 0;

    out_buf = ff_default_get_audio_buffer(s->out, nb_samples);
    if (!out_buf)
        return AVERROR(ENOMEM);

    calculate_scales(s);

    TAILQ_FOREACH(input, &s->inputs, entries){
        if (input->state & INPUT_ON) {
            if (ff_amix_input_empty(s, input->link))
                continue;

            if (input->fifo) {
                in_buf = ff_default_get_audio_buffer(s->out, nb_samples);
                if (!in_buf) {
                    av_frame_free(&out_buf);
                    return AVERROR(ENOMEM);
                }

                av_audio_fifo_read(input->fifo, (void **)in_buf->extended_data, nb_samples);
            } else {
                ret = ff_inlink_consume_samples(input->link, nb_samples, nb_samples, &in_buf);
                if (ret < 0) {
                    av_frame_free(&out_buf);
                    return ret;
                }
            }

            planar = av_sample_fmt_is_planar(out_buf->format);
            planes = planar ? out_buf->ch_layout.nb_channels : 1;
            plane_size = nb_samples * (planar ? 1 : out_buf->ch_layout.nb_channels);
            plane_size = FFALIGN(plane_size, 16);

            if (out_buf->format == AV_SAMPLE_FMT_S16 ||
                out_buf->format == AV_SAMPLE_FMT_S16P) {
                for (p = 0; p < planes; p++) {
                    vector_fmac_scalar_c((int16_t *)out_buf->extended_data[p],
                                         (int16_t *) in_buf->extended_data[p],
                                         input->scale * INT16_MAX, plane_size);
                }
            } else {
                av_log(NULL, AV_LOG_ERROR, "Unsupported sample format\n");
                av_frame_free(&out_buf);
                av_frame_free(&in_buf);
                *oframe = NULL;
                return AVERROR(ENOSYS);
            }

            av_frame_free(&in_buf);

            input->duration = av_rescale_q(out_buf->nb_samples, av_make_q(1, out_buf->sample_rate), AV_TIME_BASE_Q);

        }
        input_sync_state(input);
        if (!input->state & INPUT_ON)
            amix_input_free(input);
    }

    *oframe = out_buf;
    return nb_samples;
}