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

  char *devname;
  AlsaHandle *handles;
} AlsaSinkPriv;

static int alsasink_open(AVFilterContext *ctx, int pad)
{
    AVFilterLink *link = ctx->inputs[pad];
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink = &priv->handles[pad];
    int ret;

    if (sink->h)
        return 0;

    ret = alsa_open(sink, priv->devname, SND_PCM_STREAM_PLAYBACK,
                    link->sample_rate, link->ch_layout.nb_channels,
                    link->format);
    if (ret < 0)
        return ret;

    snd_pcm_set_volume(sink->h, priv->volume * 100);
    return ret;

}

static int alsasink_close(AVFilterContext *ctx, int pad)
{
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink = &priv->handles[pad];

    if (!sink->h)
        return 0;

    alsa_close(sink);
    return 0;
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

static void alsasink_check_resume(AlsaHandle *sink, AVFrame *frame)
{
    if (snd_pcm_state(sink->h) == SND_PCM_STATE_PAUSED) {
        sink->resume_min -= frame->nb_samples;
        if (sink->resume_min <= 0)
            snd_pcm_pause(sink->h, 0);
    }
}

static int alsasink_write_lastframe(AVFilterContext *ctx, int pad)
{
    AVFilterLink *inlink = ctx->inputs[pad];
    AlsaSinkPriv *priv = ctx->priv;
    AlsaHandle *sink = &priv->handles[pad];
    int ret;

    if (!sink->h || !sink->last_frame)
        return 0;

    alsasink_check_resume(sink, sink->last_frame);
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

    alsasink_check_resume(sink, frame);
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

    for (i = 0; i < priv->nb_inputs; i++) {
        priv->handles[i].periods = priv->periods;
        priv->handles[i].period_time = priv->period_time;
    }

    priv->volume = 1.0f;

    return 0;
}

static void alsasink_uninit(AVFilterContext *ctx)
{
    AlsaSinkPriv *priv = ctx->priv;
    av_freep(&priv->handles);
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
            if (ret < 0)
                return ret;

            ret = ff_inlink_consume_frame(inlink, &frame);
            if (ret < 0)
                continue;
            else if (ret > 0) {
                ret = alsasink_write_frame(ctx, i, frame);
                if (ret > 0)
                    ff_filter_set_ready(ctx, 100);
                continue;
            }
        }

        ff_inlink_acknowledge_status(inlink, &ret, &pts);
        if (ret >= 0)
            ff_inlink_request_frame(inlink);
        else if (ret == AVERROR_EOF) {
            alsasink_close(ctx, i);
            ret = 0;
        }
    }

    return ret;
}

static int alsasink_query_formats(const AVFilterContext *ctx,
                                AVFilterFormatsConfig **cfg_in,
                                AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    int ret, i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
        ff_formats_unref(&cfg_in[i]->formats);
        ret = ff_formats_ref(formats, &cfg_in[i]->formats);
        if (ret < 0)
            goto out;


        formats = ff_all_samplerates();
        ff_formats_unref(&cfg_in[i]->samplerates);
        ret = ff_formats_ref(formats, &cfg_in[i]->samplerates);
        if (ret < 0)
            goto out;

        layouts = ff_all_channel_counts();
        ff_channel_layouts_unref(&cfg_in[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_in[i]->channel_layouts);
        if (ret < 0)
            goto out;
    }

out:
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
        struct pollfd *poll = (struct pollfd *)res;

        for (i = 0; i < ctx->nb_inputs; i++) {
            sink = &priv->handles[i];
            if (sink->h) {
                if (snd_pcm_state(sink->h) == SND_PCM_STATE_PAUSED)
                    continue;
                snd_pcm_poll_descriptors(sink->h, &poll[ret++], 1);
            }
        }

        return ret;
    } else if (!strcmp(cmd, "poll_available")) {
        snd_pcm_sw_params_t *sw_params;

        for (i = 0; i < ctx->nb_inputs; i++) {
            sink = &priv->handles[i];
            if (sink->h) {
                ret = snd_pcm_avail_update(sink->h);
                if (ret == -EPIPE) {
                    snd_pcm_pause(sink->h, 1);
                    snd_pcm_sw_params_alloca(&sw_params);
                    snd_pcm_sw_params_current(sink->h, sw_params);
                    sink->resume_min = sw_params->avail_min;
                }
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

#define OFFSET(x) offsetof(AlsaSinkPriv, x)
#define FLAGS  AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define FLAGSR FLAGS|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption alsasink_options[] = {
    { "inputs",      "", OFFSET(nb_inputs),   AV_OPT_TYPE_INT,    { .i64 = 1 },       1, INT16_MAX, FLAGS },
    { "periods",     "", OFFSET(periods),     AV_OPT_TYPE_INT,    {.i64 = 4},         0, INT_MAX,   FLAGS },
    { "period_time", "", OFFSET(period_time), AV_OPT_TYPE_INT,    {.i64 = 20},        0, INT_MAX,   FLAGS },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING, {.str = "default"}, 0, 0,         FLAGS },
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
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
