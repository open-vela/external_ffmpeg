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
 * alsa sink
 */

#include <libavutil/avstring.h>
#include <libavutil/eval.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include "alsa.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"

typedef struct AlsaSinkPriv {
  const AVClass *class;

  int nb_inputs;
  float volume;
  int periods;
  int period_time;

  int sample_rate;                /**< sample rate */
  AVChannelLayout ch_layout;      /**< channel layout */
  enum AVSampleFormat format;     /**< sample format */

  char *devname;
  AlsaHandle *handles;
} AlsaSinkPriv;

static int alsasink_open(AVFilterContext *ctx, int pad)
{
    AVFilterLink *link = ctx->inputs[pad];
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink = &priv->handles[pad];
    int ret;

    if (sink->draining)
        return AVERROR(EAGAIN);

    if (sink->h)
        return 0;

    ret = alsa_open(sink, priv->devname, SND_PCM_STREAM_PLAYBACK,
                    link->sample_rate, link->ch_layout, link->format,
                    priv->periods, priv->period_time);
    if (ret < 0)
        return ret;

    snd_pcm_set_volume(sink->h, priv->volume * 100);
    return ret;

}

static void alsasink_drain(AVFilterContext *ctx, int pad)
{
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink = &priv->handles[pad];

    if (!sink->h)
        return;

    sink->poll_available = 0;
    sink->draining = true;
    snd_pcm_drain(sink->h);
}

static void alsasink_close(AVFilterContext *ctx, int pad)
{
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink = &priv->handles[pad];

    if (!sink->h)
        return;

    sink->draining = false;
    alsa_close(sink);
}

static void alsasink_consume_samples(AVFrame *frame, int consumed, int frame_size, int ch)
{
    int step = frame_size;
    int i;

    if (frame->data[1]) // planer
        step = frame_size / ch;

    for (i = 0; i < AV_NUM_DATA_POINTERS && frame->data[i]; i++)
      frame->data[i] += consumed * step;
    frame->nb_samples -= consumed;
}

static void alsasink_update_writable(AlsaHandle *sink, AVFrame *frame)
{
    sink->poll_available = 0;
    if (snd_pcm_state(sink->h) == SND_PCM_STATE_PAUSED)
        snd_pcm_pause(sink->h, 0);
}

static int alsasink_write_lastframe(AVFilterContext *ctx, int pad)
{
    AVFilterLink *inlink = ctx->inputs[pad];
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink = &priv->handles[pad];
    int ret;

    if (!sink->h || !sink->last_frame)
        return 0;

    alsasink_update_writable(sink, sink->last_frame);
    ret = alsa_write(sink, (void **)sink->last_frame->data,
                     sink->last_frame->nb_samples);
    if (ret < 0)
        return ret;

    alsasink_consume_samples(sink->last_frame, ret, sink->frame_size,
                             inlink->ch_layout.nb_channels);

    if (sink->last_frame->nb_samples)
        return AVERROR(EAGAIN);

    av_frame_free(&sink->last_frame);
    return 0;
}

static int alsasink_write_frame(AVFilterContext *ctx, int pad, AVFrame *frame)
{
    AVFilterLink *inlink = ctx->inputs[pad];
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink = &priv->handles[pad];
    int ret = 0;

    if (!sink->h)
        goto exit;

    if (!frame || !frame->nb_samples)
        goto exit;

    alsasink_update_writable(sink, frame);
    ret = alsa_write(sink, (void **)frame->data, frame->nb_samples);
    if (ret < 0) {
        if (ret == -EAGAIN) {
            sink->last_frame = frame;
            return AVERROR(EAGAIN);
        }
        goto exit;
    }

    if (ret != frame->nb_samples) {
        alsasink_consume_samples(frame, ret, sink->frame_size,
                                 inlink->ch_layout.nb_channels);
        sink->last_frame = frame;
        return AVERROR(EAGAIN);
    }

exit:
    av_frame_free(&frame);
    return ret;
}

static int alsasink_init(AVFilterContext *ctx)
{
    AlsaSinkPriv *priv = ctx->priv;
    int i, ret;

    for (i = 0; i < priv->nb_inputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("input%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        if ((ret = ff_append_inpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    priv->handles = av_calloc(priv->nb_inputs, sizeof(*priv->handles));
    if (!priv->handles)
        return AVERROR(ENOMEM);

    priv->volume = 1.0f;

    return 0;
}

static void alsasink_uninit(AVFilterContext *ctx)
{
    AlsaSinkPriv *priv = ctx->priv;
    int i;

    if (priv->handles) {
        for (i = 0; i < priv->nb_inputs; i++)
            alsasink_close(ctx, i);
        av_freep(&priv->handles);
    }
}

static int alsasink_activate(AVFilterContext *ctx)
{
    AlsaSinkPriv *priv = ctx->priv;
    FilterLinkInternal *li;
    AVFilterLink *inlink;
    AVFrame *frame;
    int64_t pts;
    int ret = 0;
    int i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        inlink = ctx->inputs[i];
        li = ff_link_internal(inlink);

        if (li->status_out)
            continue;

        ret = alsasink_write_lastframe(ctx, i);
        if (ret < 0)
            continue;

        if (ff_inlink_check_available_frame(inlink)) {
            ret = alsasink_open(ctx, i);
            if (ret < 0) {
                if (ret == AVERROR(EAGAIN))
                    continue;
                return ret;
            }

            ret = ff_inlink_consume_frame(inlink, &frame);
            if (ret < 0)
                continue;
            else if (ret > 0) {
                ret = alsasink_write_frame(ctx, i, frame);
                if (ret <= 0)
                    continue;
                ff_filter_set_ready(ctx, 100);
            }
        }

        ff_inlink_acknowledge_status(inlink, &ret, &pts);
        if (ret >= 0 && ff_outlink_get_status(inlink) != AVERROR_EOF && !li->frame_blocked_in)
            ff_inlink_request_frame(inlink);
        else if (ret == AVERROR_EOF) {
            alsasink_drain(ctx, i);
            ret = 0;
        }
    }

    return ret;
}

static int alsasink_query_formats(const AVFilterContext *ctx,
                                 AVFilterFormatsConfig **cfg_in,
                                 AVFilterFormatsConfig **cfg_out)
{
    AVFilterFormats *samprates = NULL, *fmts = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    AVOptionRanges *caps_ranges = NULL;
    AlsaSinkPriv *priv = ctx->priv;
    AVChannelLayout layout;
    int n, i, j, ret;

    if (priv->sample_rate > 0) {
        ret = ff_add_format(&fmts, priv->sample_rate);
        if (ret < 0)
            goto out;
    } else {
        ret = alsa_query_caps(&caps_ranges, priv->devname, "sample_rates", true);
        if (ret >= 0) {
            for (n = 0; n < caps_ranges->nb_ranges; n++) {
                ret = ff_add_format(&fmts, caps_ranges->range[n]->value_min);
                if (ret < 0)
                    goto out;
            }
            av_opt_freep_ranges(&caps_ranges);
        }
    }
    samprates = fmts;
    fmts = NULL;

    if (priv->format != AV_SAMPLE_FMT_NONE) {
        ret = ff_add_format(&fmts, priv->format);
        if (ret < 0)
            goto out;
    } else {
        ret = alsa_query_caps(&caps_ranges, priv->devname, "sample_fmts", true);
        if (ret >= 0) {
            for (n = 0; n < caps_ranges->nb_ranges; n++) {
                ret = ff_add_format(&fmts, caps_ranges->range[n]->value_min);
                if (ret < 0)
                    goto out;
            }
            av_opt_freep_ranges(&caps_ranges);
        }
    }

    if (priv->ch_layout.nb_channels > 0) {
        ret = ff_add_channel_layout(&layouts, &priv->ch_layout);
        if (ret < 0)
            goto out;
    } else {
        ret = alsa_query_caps(&caps_ranges, priv->devname, "channels", true);
        if (ret >= 0) {
            for (n = 0; n < caps_ranges->nb_ranges; n++) {
                int min_ch = caps_ranges->range[n]->value_min;
                int max_ch = caps_ranges->range[n]->is_range ?
                             caps_ranges->range[n]->value_max : min_ch;

                for (j = min_ch; j <= max_ch; j++) {
                    av_channel_layout_default(&layout, j);
                    ret = ff_add_channel_layout(&layouts, &layout);
                    if (ret < 0)
                        goto out;
                }
            }
            av_opt_freep_ranges(&caps_ranges);
        }
    }

    for (i = 0; i < ctx->nb_inputs; i++) {
        ff_formats_unref(&cfg_in[i]->formats);
        ret = ff_formats_ref(fmts, &cfg_in[i]->formats);
        if (ret < 0)
            goto out;

        ff_formats_unref(&cfg_in[i]->samplerates);
        ret = ff_formats_ref(samprates, &cfg_in[i]->samplerates);
        if (ret < 0)
            goto out;

        ff_channel_layouts_unref(&cfg_in[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_in[i]->channel_layouts);
        if (ret < 0)
            goto out;
    }

out:
    av_opt_freep_ranges(&caps_ranges);
    ff_channel_layouts_unref(&layouts);
    ff_formats_unref(&samprates);
    ff_formats_unref(&fmts);

    return ret;
}

static int alsasink_process_command(AVFilterContext *ctx,
                                    const char *cmd, const char *args,
                                    char *res, int res_len, int flags)
{
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink;
    int ret = 0;
    int i;

    if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll;
        int max_fds;

        if (!res || res_len < sizeof(struct pollfd))
            return AVERROR(EINVAL);

        poll = (struct pollfd *)res;
        max_fds = res_len / sizeof(struct pollfd);

        for (i = 0; i < ctx->nb_inputs; i++) {
            sink = &priv->handles[i];
            if (sink->h) {
                if (snd_pcm_state(sink->h) == SND_PCM_STATE_PAUSED)
                    continue;

                if (ret >= max_fds) {
                    av_log(ctx, AV_LOG_WARNING, "Insufficient poll fd buffer space: "
                           "need at least %d fds but only %d available, skipping remaining inputs\n",
                           ret + 1, max_fds);
                    break;
                }

                snd_pcm_poll_descriptors(sink->h, &poll[ret], 1);

                if (sink->poll_available >= sink->periods) {
                    if (snd_pcm_state(sink->h) == SND_PCM_STATE_PREPARED)
                        continue;

                    poll[ret].events = POLLERR;
                }

                ret++;
            }
        }

        return ret;
    } else if (!strcmp(cmd, "poll_available")) {
        snd_pcm_sw_params_t *sw_params;
        snd_pcm_state_t state;

        for (i = 0; i < ctx->nb_inputs; i++) {
            sink = &priv->handles[i];
            if (!sink->h)
                continue;

            state = snd_pcm_state(sink->h);
            if (state == SND_PCM_STATE_PAUSED)
                continue;

            if (sink->draining)
                snd_pcm_drain(sink->h);
            else
                sink->poll_available++;

            snd_pcm_avail_update(sink->h);
            if (state == SND_PCM_STATE_XRUN) {
                snd_pcm_pause(sink->h, 1);
                sink->poll_available = 0;
            } else if (state == SND_PCM_STATE_SETUP) {
                if (sink->draining)
                    alsasink_close(ctx, i);
            }
        }

        ff_filter_set_ready(ctx, 100);
        return 0;
    } else if (!strcmp(cmd, "set_parameter")) {
        alsa_set_parameter(priv->devname, args);
        return 0;
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int alsasink_forward_command(AVFilterContext *ctx,
    int pad_idx, const char* target, const char *cmd,
    const char *arg, char *res, int res_len, int flags)
{
    AlsaSinkPriv *priv = ctx->priv;
    snd_pcm_sframes_t frame_count;
    FilterLinkInternal* li;
    int64_t latency = 0;
    AVFilterLink *link;
    int i, nb_frames;
    AlsaHandle *sink;
    AVFrame *frame;
    int ret = 0;

    av_log(ctx, AV_LOG_DEBUG, "Forwarding command '%s'\n", cmd);

    if (!strcmp(cmd, "latency")) {
        if (!res || res_len < sizeof(int64_t) || pad_idx >= ctx->nb_inputs || pad_idx < 0)
            return AVERROR(EINVAL);

        link = ctx->inputs[pad_idx];
        li = ff_link_internal(link);

        nb_frames = ff_framequeue_queued_frames(&li->fifo);
        for (i = 0; i < nb_frames; i++) {
            frame = ff_framequeue_peek(&li->fifo, i);
            if (frame)
                latency += av_rescale_q(frame->duration, frame->time_base, AV_TIME_BASE_Q);
        }

        sink = &priv->handles[pad_idx];
        if (!sink->h || snd_pcm_state(sink->h) != SND_PCM_STATE_RUNNING) {
            *(int64_t*)res = latency;
            return 0;
        }

        ret = snd_pcm_delay(sink->h, &frame_count);
        if (ret < 0 || frame_count < 0) {
            av_log(ctx, AV_LOG_ERROR, "Error getting delay: %s\n", snd_strerror(ret));
            return ret;
        }

        latency += av_rescale(frame_count, AV_TIME_BASE, sink->sample_rate);
        *(int64_t*)res = latency;
        return 0;
    }

    return 0;
}

#define OFFSET(x) offsetof(AlsaSinkPriv, x)
#define FLAGS  AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define FLAGSR FLAGS|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption alsasink_options[] = {
    { "inputs",      "", OFFSET(nb_inputs),   AV_OPT_TYPE_INT,        { .i64 = 1 },                1, INT16_MAX, FLAGS },
    { "periods",     "", OFFSET(periods),     AV_OPT_TYPE_INT,        {.i64 = 4},                  0, INT_MAX,   FLAGS },
    { "period_time", "", OFFSET(period_time), AV_OPT_TYPE_INT,        {.i64 = 20},                 0, INT_MAX,   FLAGS },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     {.str = "default"},          0, 0,         FLAGS },
    { "format",      "", OFFSET(format),      AV_OPT_TYPE_SAMPLE_FMT, {.i64 =AV_SAMPLE_FMT_NONE}, -1, INT_MAX,   FLAGSR },
    { "sample_rate", "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                  0, INT_MAX,   FLAGSR },
    { "ch_layout",   "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},               0, 0,         FLAGSR },
    { NULL },
};

AVFILTER_DEFINE_CLASS(alsasink);

const AVFilter ff_asink_alsasink = {
    .name            = "alsasink",
    .description     = NULL_IF_CONFIG_SMALL("Alsa sink"),
    .priv_class      = &alsasink_class,
    .priv_size       = sizeof(AlsaSinkPriv),
    .init            = alsasink_init,
    .uninit          = alsasink_uninit,
    .activate        = alsasink_activate,
    FILTER_QUERY_FUNC2(alsasink_query_formats),
    .process_command = alsasink_process_command,
    .forward_command = alsasink_forward_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
