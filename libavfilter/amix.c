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
#define INPUT_BLOCKED  4    /**< input is blocked (no frame available now) */

typedef struct AMixInput {
    AMixContext *parent;        /**< amix context */
    float scale;                /**< scale for each input */
    uint8_t state;              /**< current state of each input */
    int mix_size;               /**< size of last mix */
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
    int frame_size;         /**< out frame size */
};

static inline bool input_needs_detach(AMixInput *input)
{
    return input->state & INPUT_EOF || input->state & INPUT_BLOCKED;
}

/**
 * Update the scaling factors to apply to each input during mixing.
 *
 * Only the inputs which are in INPUT_ON state and non-empty need mix.
 * Cause input with frame_blocked_in won't be removed from amix context,
 * we need take care of this catiously.
 */
static int calculate_scales(AMixContext *s)
{
    int activate_inputs = 0;
    AMixInput *input;
    int i;

    if (TAILQ_EMPTY(&s->inputs))
        return 0;

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (input->state & INPUT_ON)
            activate_inputs++;
    }

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (input->state & INPUT_ON) {
            input->scale = 1.0 / activate_inputs;
        }
    }

    return activate_inputs;
}

/**
 * Calculate the num of inputs which are in INPUT_ON and not blocked in.
 *
 * This function is used to check how many inputs are not in draining.
 */
static int calc_active_inputs(AMixContext *s)
{
    AMixInput *input;
    int count = 0;

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (!input_needs_detach(input))
            count++;
    }

    return count;
}

static int get_output_samples(AMixContext *s)
{
    int nb_samples = INT_MAX, unblocked_samples = INT_MAX;
    int min_samples = INT_MAX, max_samples = 0;
    AMixInput *input;
    int count = 0;
    int ns;

    if (TAILQ_EMPTY(&s->inputs))
        return 0;

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (input->state & INPUT_ON) {
            if (!input->fifo)
                ns = ff_inlink_queued_samples(input->link);
            else
                ns = av_audio_fifo_size(input->fifo);

            nb_samples = FFMIN(nb_samples, ns);
            if (!(input->state & INPUT_BLOCKED)) {
                if (input->state & INPUT_EOF)
                    max_samples = FFMAX(max_samples, ns);
                else
                    min_samples = FFMIN(min_samples, ns);
                unblocked_samples = FFMIN(unblocked_samples, ns);
            } else {
                /** BLOCKED inputs should drain all its samples, especially when all inputs are in BLOCKED. */

                max_samples = FFMAX(max_samples, ns);
            }
        }
    }

    /**
     * If specific mix size is given, then the following logic depends
     * whether to mix and which inputs need to be mixed.
     *
     * nb_samples: minimum samples among all inputs.
     * min_samples: minimum samples among all active inputs with states
     *              equal to INPUT_ON.
     * max_samples: maximum samples among all drain inputs with
     *              INPUT_EOF or INPUT_BLOCKED state.
     * unblocked_samples: minimum samples among all unblocked inputs.
     *
     * Here are three cases that need to be considered:
     * - Partially draining: at least one but not all inpputs with
     *                       state equal to INPUT_ON;
     * - None draining: all inpputs' state equal to INPUT_ON;
     * - All draining: none input's state equals to INPUT_ON.
     *
     */

    if (s->frame_size) {
        count = calc_active_inputs(s);

        if (count > 0 && count < s->nb_inputs) {// partially draining

             /**
              * If min_samples more than given size,
              * should mix them and fill silence data in other drain inputs,
              * otherwise wait.
              * */

            if (min_samples >= s->frame_size)
                nb_samples = s->frame_size;
        } else if (count == s->nb_inputs) { // none draining

            /* If nb_samples more than given size, should mix them, otherwise wait. */

            if (nb_samples >= s->frame_size)
                nb_samples = s->frame_size;
        } else if (count == 0 && max_samples > 0) { // all draining

            /* If max_samples more than zero, mix them and fill silence data to frame_size. */

            nb_samples = s->frame_size;
        }

        if (nb_samples != s->frame_size)
            return 0;
        return nb_samples;
    }

    return nb_samples ? nb_samples : unblocked_samples;
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
    input->mix_size = 1; // initialize the water level with a non-zero value to require data when there is no data in fifo or link.
    input->state = INPUT_ON;

    TAILQ_INSERT_TAIL(&s->inputs, input, entries);
    s->nb_inputs++;

    av_log(NULL, AV_LOG_INFO, "[%s %d]input %p alloc, resample: %d, first size: %d total inputs:%d\n", __func__, __LINE__, input, input->fifo ? 1: 0, ff_inlink_queued_samples(link) ,s->nb_inputs);

    return input;

err:
    if (input->fifo)
        av_audio_fifo_free(input->fifo);
    av_freep(&input);
    return NULL;
}

static void amix_input_free(AMixInput *in)
{
    int link_size = 0;
    int fifo_size = 0;
    AMixContext *s;

    if (!in)
        return;

    s = in->parent;

    TAILQ_REMOVE(&s->inputs, in, entries);

    link_size = ff_inlink_queued_samples(in->link);

    if (in->resample && in->fifo) {
        fifo_size = av_audio_fifo_size(in->fifo);
        av_audio_fifo_free(in->fifo);
        in->fifo = NULL;
        ff_resample_uninit(in->resample);
        av_freep(&in->resample);
    }

    av_free(in);
    s->nb_inputs--;

    av_log(NULL, AV_LOG_INFO, "[%s %d]input %p free, link_size:%d fifo_size:%d total inputs:%d\n",
        __func__, __LINE__, in, link_size, fifo_size, s->nb_inputs);
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
 * case1: queued_samples > 0, and link status is AVERROR_EOF (draining), input->state |= INPUT_EOF;
 *
 * case2: queued_samples == 0, and link status is AVERROR_EOF, input->state &= ~INPUT_ON;
 *
 * case3: link blocked, and samples < mix_size, input->state |= INPUT_BLOCKED;
 *
 * case4: link unblocked, and samples >= mix_size, input->state &= ~INPUT_BLOCKED;
 *
 */
static void input_sync_state(AMixInput *input)
{
    AMixContext *s = input->parent;
    AVFilterLink *link = input->link;
    FilterLinkInternal *li;

    if (ff_outlink_get_status(link) == AVERROR_EOF) {

        /* If a link is blocked and was closing at last, then free it after being drained. */

        input->state &= ~INPUT_BLOCKED;
        input->state |= INPUT_EOF;
    } else {
        li = ff_link_internal(input->link);
        if (li->frame_blocked_in && ((input->fifo ? av_audio_fifo_size(input->fifo) : ff_inlink_queued_samples(input->link)) < input->mix_size)) {
            if (!(input->state & INPUT_BLOCKED))
                av_log(NULL, AV_LOG_INFO, "input %p blocking, link size:%d fifo size:%d nb_inputs:%d\n",
                    input, ff_inlink_queued_samples(input->link), input->fifo ? av_audio_fifo_size(input->fifo) : 0, s->nb_inputs);
            input->state |= INPUT_BLOCKED;
        } else if (s->frame_size && !li->frame_blocked_in) {
            if ((input->fifo ? av_audio_fifo_size(input->fifo) : ff_inlink_queued_samples(input->link)) >= input->mix_size) {
                if (input->state & INPUT_BLOCKED)
                    av_log(NULL, AV_LOG_INFO, "input %p unblock from blocking, link size:%d fifo size:%d nb_inputs:%d\n",
                        input, ff_inlink_queued_samples(input->link), input->fifo ? av_audio_fifo_size(input->fifo) : 0, s->nb_inputs);
                input->state &= ~INPUT_BLOCKED;
            }
        } else if (!li->frame_blocked_in)
            input->state &= ~INPUT_BLOCKED;
    }

    if (input->state & INPUT_EOF && ff_amix_input_empty(s, link))
        input->state &= ~INPUT_ON;
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

bool ff_amix_input_empty(AMixContext *s, AVFilterLink *link)
{
    AMixInput *input;

    if (!s)
        return true;

    input = amix_find_input(s, link);

    return !input || !input->link || (input->fifo ? av_audio_fifo_size(input->fifo) == 0 : !ff_inlink_check_available_samples(input->link, 1));
}

bool ff_amix_input_want(AMixContext *s, AVFilterLink *link)
{
    AMixInput *input;
    int size;

    if (!s)
        return false;

    input = amix_find_input(s, link);
    if (!input)
        return true;

    if (input_needs_detach(input))
        return false;

    if (input->link) {
        if (s->frame_size)
            size = s->frame_size;
        else
            size = input->mix_size;

        return (ff_outlink_get_status(input->link) != AVERROR_EOF && (input->fifo ? av_audio_fifo_size(input->fifo) : ff_inlink_queued_samples(input->link)) < size);
    }

    return true;
}

/**
 * Check if all inputs are in state INPUT_EOF | INPUT_BLOCKED.
 */
bool ff_amix_blocked(AMixContext *s)
{
    AMixInput *input;

    if (!s)
        return false;

    if (TAILQ_EMPTY(&s->inputs))
        return true;

    TAILQ_FOREACH(input, &s->inputs, entries) {
        if (!input_needs_detach(input))
            return false;
    }

    return true;
}

/* Where param "link" is a real link. */
int ff_amix_input_write(AMixContext *s, AVFilterLink *link)
{
    AVFrame *frame = NULL, *rframe = NULL;
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
            if (ret <= 0) {
                av_frame_free(&frame);
                return ret;
            }

            ret = av_audio_fifo_write(input->fifo, (void **)rframe->extended_data, rframe->nb_samples);
            if (ret < 0) {
                av_frame_free(&rframe);
                return ret;
            }
            av_frame_free(&rframe);
            av_frame_free(&frame);
        }
    }

    return 0;
}


/**
 * Check whether all data on the link has been written
 * to amix through the ff_amix_input_write API.
 */
bool ff_amix_input_write_down(AMixContext *s, AVFilterLink *link)
{
    AMixInput *input;

    if (!s)
        return false;

    input = amix_find_input(s, link);

    return !input || !input->link || (input->fifo ? !ff_inlink_check_available_samples(input->link, 1) : true);
}

/**
 * Mixing data and output in frame.
 *
 * If the output framesize is limited by codec like libopus_encoder, here are two cases
 * we should care:
 * case1: one of the inputs is in drain, and its data size below the output framesize;
 * case2: all the inputs are in drain or the last active input in drain, and the data
 *        size below the output framesize;
 *
 * For case1, we should fill the drain input frame to given frame size;
 * For case2, frame can be given as last frame without filled to framesize if the codec
 * support AV_CODEC_CAP_SMALL_LAST_FRAME, otherwise filling is also needed.
 *
 */
int ff_amix_read(AMixContext *s, AVFrame **oframe)
{
    int planes, plane_size, p, planar;
    AVFrame *out_buf = NULL, *in_buf = NULL;
    int  i, ret, nb_samples;
    AMixInput *input, *tinput;
    int64_t pts;
    int scales;

    if (!s)
        return AVERROR(EINVAL);

    if (TAILQ_EMPTY(&s->inputs))
        return AVERROR(EAGAIN);

    TAILQ_FOREACH_SAFE(input, &s->inputs, entries, tinput) {
        input_sync_state(input);
        if (!(input->state & INPUT_ON))
            amix_input_free(input);
    }

    nb_samples = get_output_samples(s);
    if (nb_samples == INT_MAX || nb_samples == 0)
        return 0;

    out_buf = ff_default_get_audio_buffer(s->out, nb_samples);
    if (!out_buf)
        return AVERROR(ENOMEM);

    scales = calculate_scales(s);

    TAILQ_FOREACH_SAFE(input, &s->inputs, entries, tinput) {
        if (input->state & INPUT_ON) {
            int left_size;
            left_size = input->fifo ? av_audio_fifo_size(input->fifo) : ff_inlink_queued_samples(input->link);

            /* If input no samples left, then skip mix. */

            if (left_size == 0)
                continue;

            if (input_needs_detach(input) && left_size < nb_samples) {

                av_log(NULL, AV_LOG_INFO, "input %p state: %d size: %d silence size: %d nb_inputs:%d\n",
                    input, input->state, left_size, nb_samples - left_size, s->nb_inputs);

                in_buf = ff_default_get_audio_buffer(s->out, nb_samples);
                if (!in_buf) {
                    ret = AVERROR(ENOMEM);
                    goto err;
                }

                if (input->fifo)
                    av_audio_fifo_read(input->fifo, (void **)in_buf->extended_data, left_size);
                else {
                    AVFrame *tmp;
                    ret = ff_inlink_consume_samples(input->link, left_size, left_size, &tmp);
                    if (ret < 0)
                        goto err;
                    if ((ret = av_samples_copy(in_buf->extended_data, tmp->extended_data, 0, 0,
                                              tmp->nb_samples, tmp->ch_layout.nb_channels,
                                              tmp->format)) < 0) {
                        av_frame_free(&tmp);
                        goto err;
                    }
                    av_frame_free(&tmp);
                }

                av_samples_set_silence(in_buf->extended_data, left_size, nb_samples - left_size,
                                      s->out->ch_layout.nb_channels, s->out->format);
            } else {
                if (input->fifo) {
                    in_buf = ff_default_get_audio_buffer(s->out, nb_samples);
                    if (!in_buf) {
                        ret = AVERROR(ENOMEM);
                        goto err;
                    }

                    av_audio_fifo_read(input->fifo, (void **)in_buf->extended_data, nb_samples);
                } else {
                    ret = ff_inlink_consume_samples(input->link, nb_samples, nb_samples, &in_buf);
                    if (ret < 0)
                        goto err;
                }
            }

            if (scales == 1) {
                if (av_frame_copy(out_buf, in_buf) < 0) {
                    av_frame_free(&out_buf);
                    return AVERROR(EINVAL);
                }
            } else {
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
                    av_log(input->link->dst, AV_LOG_ERROR, "Unsupported sample format\n");
                    ret = AVERROR(ENOSYS);
                    goto err;
                }
            }

            av_frame_free(&in_buf);

            input->mix_size = out_buf->nb_samples;
        }

        input_sync_state(input);
        if (!(input->state & INPUT_ON))
            amix_input_free(input);
    }

    *oframe = out_buf;
    return nb_samples;

err:
    if (in_buf)
        av_frame_free(&in_buf);
    av_frame_free(&out_buf);
    return ret;
}

int ff_amix_set_frame_size(AMixContext *s, int frame_size)
{
    return s->frame_size = frame_size;
}