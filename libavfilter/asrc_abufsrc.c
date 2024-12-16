/*
 * Copyright (c) 2024 HiccupZhu
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
 * memory buffer source filter
 */

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/frame.h"
#include "libavutil/internal.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"
#include "aresample.h"

#define SUGGESTED_NB_SAMPLES 1024

typedef struct BufferSourceContext {
    const AVClass    *class;
    unsigned          nb_failed_requests;
    int               nb_outputs;

    AResampleContext *aresamples;

    int (*on_event_cb)(void *udata, int evt, int64_t args);
    void *on_event_cb_udata;
} BufferSourceContext;

static int attribute_align_arg abufsrc_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    BufferSourceContext *s = ctx->priv;
    AVFrame *copy;
    int i, ret;

    s->nb_failed_requests = 0;

    if (!frame) {
        for (i = 0; i < ctx->nb_outputs; i++) {
            AVFilterLink *outlink = ctx->outputs[i];
            AVFrame *frame = av_frame_alloc();
            if (!frame)
                return AVERROR(ENOMEM);

            frame->format = outlink->format;
            frame->sample_rate = outlink->sample_rate;
            av_channel_layout_copy(&frame->ch_layout, &outlink->ch_layout);

            ret = ff_filter_frame(ctx->outputs[i], frame);
            if (ret < 0) {
                av_log(ctx, AV_LOG_WARNING, "outputlink[%d] out_frame failed ret:%d:%s\n", i, ret, av_err2str(ret));
            }
        }

        return 0;
    }

    for (i = 0; i < ctx->nb_outputs; i++) {
        AVFilterLink *outlink = ctx->outputs[i];
        ret = ff_resample_frame(&s->aresamples[i], outlink, frame, &copy);
        if (ret <= 0) {
            copy = av_frame_clone(frame);
            if (!copy)
                return AVERROR(ENOMEM);
        }

        ret = ff_filter_frame(ctx->outputs[i], copy);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int av_cold abufsrc_set_event_cb(AVFilterContext *ctx,
    int (*on_event_cb)(void *udata, int evt, int64_t args), void *udata)
{
    BufferSourceContext *s = ctx->priv;
    FilterLinkInternal *li = ff_link_internal(ctx->outputs[0]);
    int i, ret;

    s->on_event_cb = on_event_cb;
    s->on_event_cb_udata = udata;

    if (s->on_event_cb) {
        li->frame_wanted_out = 1;
        ff_filter_set_ready(ctx, 100);
    } else {
        ret = abufsrc_send_frame(ctx, NULL);
        if (ret < 0)
            return ret;

        for (i = 0; i < s->nb_outputs; i++)
            ff_resample_uninit(&s->aresamples[i]);
    }

    return 0;
}

static int config_props(AVFilterLink *link)
{
    return 0;
}

static av_cold int init_audio(AVFilterContext *ctx)
{
    BufferSourceContext *s = ctx->priv;
    char buf[128];
    int i, ret = 0;

    for (i = 0; i < s->nb_outputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        pad.config_props = config_props;
        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    s->aresamples = av_mallocz(s->nb_outputs * sizeof(*s->aresamples));
    if (!s->aresamples)
        return AVERROR(ENOMEM);

    return ret;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    BufferSourceContext *s = ctx->priv;
    int i;

    for (i = 0; i < s->nb_outputs; i++)
        ff_resample_uninit(&s->aresamples[i]);
    if (s->aresamples)
        av_freep(&s->aresamples);
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP,
        AV_SAMPLE_FMT_DBL, AV_SAMPLE_FMT_DBLP,
        AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P,
        AV_SAMPLE_FMT_NONE
    };
    const BufferSourceContext *c = ctx->priv;
    int i, ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ctx->outputs[i]->type != AVMEDIA_TYPE_AUDIO) {
            av_log((void*)ctx, AV_LOG_ERROR, "Output[%d]:%s media-type mismatch\n", i, av_get_media_type_string(ctx->outputs[i]->type));
            return AVERROR(EINVAL);
        }
    }

    if ((ret = ff_set_common_formats2(ctx, cfg_in, cfg_out, ff_make_format_list(sample_fmts))) < 0)
        return ret;

    if ((ret = ff_set_common_samplerates2(ctx, cfg_in, cfg_out, ff_all_samplerates())) < 0)
        return ret;

    if ((ret = ff_set_common_channel_layouts2(ctx, cfg_in, cfg_out, ff_all_channel_counts())) < 0)
        return ret;

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    BufferSourceContext *c = ctx->priv;
    AVFrame *frame;
    int i, ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (!ff_outlink_frame_wanted(ctx->outputs[i])) {
            c->nb_failed_requests++;
            return FFERROR_NOT_READY;
        }
    }

    if (!c->on_event_cb)
        return 0;

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    frame->nb_samples = SUGGESTED_NB_SAMPLES;
    ret = c->on_event_cb(c->on_event_cb_udata, 0, (intptr_t)frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    ret = abufsrc_send_frame(ctx, frame);
    av_frame_free(&frame);
    if (ret < 0)
        return ret;

    return 0;
}

static int abufsrc_proccess_command(AVFilterContext *ctx, const char *cmd, const char *args,
    char *res, int res_len, int flags)
{
    BufferSourceContext *s = ctx->priv;
    int ret;

    if (!cmd)
        return AVERROR(EINVAL);

    av_log(ctx, AV_LOG_INFO, "cmd:%s args:%s\n", cmd, args);
    if (!av_strcasecmp(cmd, "link")) {
        int (*on_event_cb)(void *udata, int evt, int64_t args);
        void *udata;

        if (!args)
            return AVERROR(EINVAL);

        if (sscanf(args, "%p %p", &on_event_cb, &udata) != 2)
            return AVERROR(EINVAL);

        ret = abufsrc_set_event_cb(ctx, on_event_cb, udata);
        if (ret < 0)
            return ret;

        return 0;
    } else if (!av_strcasecmp(cmd, "unlink")) {
        ret = abufsrc_set_event_cb(ctx, NULL, NULL);
        if (ret < 0)
            return ret;

        return 0;
    }

    return AVERROR(ENOSYS);
}

#define OFFSET(x) offsetof(BufferSourceContext, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM

static const AVOption abuffer_options[] = {
    { "outputs", "set number of outputs", OFFSET(nb_outputs), AV_OPT_TYPE_INT,   { .i64 = 1 }, 1, INT_MAX, A },
    { NULL },
};

AVFILTER_DEFINE_CLASS(abuffer);

const AVFilter ff_asrc_abufsrc = {
    .name          = "abufsrc",
    .description   = NULL_IF_CONFIG_SMALL("Buffer audio frames, and make them accessible to the filterchain."),
    .priv_size     = sizeof(BufferSourceContext),
    .activate  = activate,
    .init      = init_audio,
    .uninit    = uninit,

    .inputs    = NULL,
    FILTER_QUERY_FUNC2(query_formats),
    .priv_class = &abuffer_class,
    .flags = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .process_command = abufsrc_proccess_command,
};
