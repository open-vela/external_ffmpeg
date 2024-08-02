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

#include "config_components.h"

#include "libavutil/thread.h"
#include "libavcodec/avcodec.h"
#include "packet_wrapper.h"
#include "filters.h"

typedef struct DecoderContext {
    const AVClass   *class;
    int64_t         next_pts;
    FFFrameQueue    in_queue;
    FFFrameQueue    out_queue;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    pthread_t       avtdec_id;
    bool            thread_exit;
    int             priority;
    int             stack_size;
    int             icnt;
    int             ocnt;
} DecoderContext;

#define OFFSET(x) offsetof(DecoderContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption avtdec_options[]= {
    { "priority",  "priority of decoder thread",             OFFSET(priority),   AV_OPT_TYPE_INT, {.i64 = 100 },   0, INT_MAX, FLAGS },
    { "stack_size", "stack size of decoder thread",          OFFSET(stack_size), AV_OPT_TYPE_INT, {.i64 = 61440 }, 0, INT_MAX, FLAGS },
    { "icnt",      "maximum number of decoder input packet", OFFSET(icnt),       AV_OPT_TYPE_INT, {.i64 = 6 },     0, INT_MAX, FLAGS },
    { "ocnt",      "maximum number of decoder output frame", OFFSET(ocnt),       AV_OPT_TYPE_INT, {.i64 = 2 },     0, INT_MAX, FLAGS },
    { NULL },
};

static int avtdec_open(AVFilterContext *ctx, AVCodecContext **codec_ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    DecoderContext *priv = ctx->priv;
    AVCodecParameters *param;
    const AVCodec *codec;
    AVFrame *frame = NULL;
    int ret;

    pthread_mutex_lock(&priv->mutex);
    if (ff_framequeue_queued_frames(&priv->in_queue)) {
        frame = ff_framequeue_peek(&priv->in_queue, 0);
    }
    pthread_mutex_unlock(&priv->mutex);

    if (!frame || !frame->opaque_ref) {
        av_log(ctx, AV_LOG_ERROR, "Lack negotiation parameter.\n");
        return AVERROR(EINVAL);
    }

    unwrap_frame(frame, NULL, &param);
    codec = avcodec_find_decoder(param->codec_id);
    if (!codec) {
        av_log(ctx, AV_LOG_ERROR, "Failed find codec.\n");
        return AVERROR(EINVAL);
    }

    *codec_ctx = avcodec_alloc_context3(codec);
    if (!*codec_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Failed malloc codec_ctx.\n");
        return AVERROR(ENOMEM);
    }

    ret = avcodec_parameters_to_context(*codec_ctx, param);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed copy param to codec_ctx.\n");
        goto out;
    }

    (*codec_ctx)->time_base    = inlink->time_base;
    (*codec_ctx)->thread_count = ff_filter_get_nb_threads(ctx);
    ret = avcodec_open2(*codec_ctx, codec, &frame->metadata);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed open codec %d %s.\n", ret, av_err2str(ret));
        goto out;
    }

    priv->next_pts = AV_NOPTS_VALUE;
    return 0;

out:
    avcodec_free_context(codec_ctx);
    return ret;
}

static void avtdec_close(AVFilterContext *ctx, AVCodecContext **codec_ctx)
{
    DecoderContext *priv = ctx->priv;
    AVFrame frame;
    int ret;

    pthread_mutex_lock(&priv->mutex);
    ff_framequeue_free(&priv->in_queue);
    ff_framequeue_free(&priv->out_queue);
    pthread_mutex_unlock(&priv->mutex);

    if (*codec_ctx) {
        ret = avcodec_send_packet(*codec_ctx, NULL);

        memset(&frame, 0, sizeof(AVFrame));
        while(1) {
            ret = avcodec_receive_frame(*codec_ctx, &frame);
            if (ret < 0) break;
            av_frame_unref(&frame);
        }

        avcodec_close(*codec_ctx);
        avcodec_free_context(codec_ctx);
    }
}

static int avtdec_send(AVCodecContext *codec_ctx, AVFrame *frame)
{
    AVPacket *pkt;
    int ret;

    unwrap_frame(frame, &pkt, NULL);
    if (pkt && (pkt->flags & AV_PKT_FLAG_EVT_EOS))
        pkt = NULL;

    ret = avcodec_send_packet(codec_ctx, pkt);
    if (ret < 0) {
        /* ignore AVERROR_INVALIDDATA, and try again */
        if (ret == AVERROR_INVALIDDATA)
            ret = AVERROR(EAGAIN);
    }

    av_frame_free(&frame);
    return ret;
}

static int avtdec_receive(AVFilterContext *ctx, AVCodecContext *codec_ctx, AVFrame **out)
{
    DecoderContext *priv = ctx->priv;
    AVFrame *frame;
    int ret;

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    ret = avcodec_receive_frame(codec_ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    if (codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (frame->pts == AV_NOPTS_VALUE && priv->next_pts != AV_NOPTS_VALUE)
            frame->pts = priv->next_pts;

        if (frame->pts != AV_NOPTS_VALUE)
            priv->next_pts = frame->pts + frame->nb_samples;
    }

    frame->time_base = ctx->inputs[0]->time_base;
    *out = frame;
    return 0;
}

static av_cold int avtdec_init(AVFilterContext *ctx)
{
    DecoderContext *priv = ctx->priv;

    pthread_mutex_init(&priv->mutex, NULL);
    pthread_cond_init(&priv->cond, NULL);
    priv->thread_exit = true;
    priv->avtdec_id = -1;

    return 0;
}

static av_cold void avtdec_uninit(AVFilterContext *ctx)
{
    DecoderContext *priv = ctx->priv;

    pthread_mutex_destroy(&priv->mutex);
    pthread_cond_destroy(&priv->cond);
}

static AVFilterFormats *ff_all_decoders(enum AVMediaType type)
{
    const AVCodec *codec = NULL;
    void *iterate = NULL;
    AVFilterFormats *ffmts = NULL;
    int ret;

    while (codec = av_codec_iterate(&iterate)) {
        if (codec->type != type || av_codec_is_encoder(codec))
            continue;

        if (ret = ff_add_format(&ffmts, codec->id) < 0)
            goto out;
    }

    return ffmts;
out:
    ff_formats_unref(&ffmts);
    return NULL;
}

static int avtdec_query_formats(AVFilterContext *ctx)
{
    AVFilterLink *inlink  = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterFormats *ffmts = NULL;
    AVFilterChannelLayouts *ffcls = NULL;
    int ret = 0;

    ffmts = inlink->incfg.codecs ? inlink->incfg.codecs : ff_all_decoders(inlink->type);
    if ((ret = ff_formats_ref(ffmts, &inlink->outcfg.codecs)) < 0)
        goto out;

    if ((ret = ff_formats_ref(ff_all_raw_codecs(outlink->type), &outlink->incfg.codecs)) < 0)
        goto out;

    ffmts = inlink->incfg.formats ? inlink->incfg.formats : ff_all_formats(outlink->type);
    if ((ret = ff_set_common_formats(ctx, ffmts)) < 0)
        goto out;

    if (inlink->type == AVMEDIA_TYPE_AUDIO) {
        ffmts = inlink->incfg.samplerates ? inlink->incfg.samplerates : ff_all_samplerates();
        if ((ret = ff_set_common_samplerates(ctx, ffmts)) < 0)
            goto out;

        ffcls = inlink->incfg.channel_layouts ? inlink->incfg.channel_layouts : ff_all_channel_layouts();
        if ((ret = ff_set_common_channel_layouts(ctx, ffcls)) < 0)
            goto out;
    }

    return 0;

out:
    if (ffmts != inlink->incfg.formats
        && ffmts != inlink->incfg.samplerates
        && ffmts != inlink->incfg.codecs)
        ff_formats_unref(&ffmts);

    if (ffcls != inlink->incfg.channel_layouts)
        ff_channel_layouts_unref(&ffcls);

    return ret;
}

static int avtdec_forward_command(AVFilterContext *ctx,
                                  int pad_idx, const char *target, const char *cmd,
                                  const char *arg, char *res, int res_len, int flags)
{
    DecoderContext *priv = ctx->priv;
    AVFrame *frame;
    int ret;

    if (!strcmp(cmd, "flush")) {
        pthread_mutex_lock(&priv->mutex);
        if (!priv->thread_exit) {
            while (ff_framequeue_queued_frames(&priv->in_queue)) {
                frame = ff_framequeue_take(&priv->in_queue);
                av_frame_free(&frame);
            }
            while (ff_framequeue_queued_frames(&priv->out_queue)) {
                frame = ff_framequeue_take(&priv->out_queue);
                av_frame_free(&frame);
            }

            frame = av_frame_alloc();
            if (!frame) {
                pthread_mutex_unlock(&priv->mutex);
                return AVERROR(ENOMEM);
            }

            ff_framequeue_add(&priv->in_queue, frame);
            pthread_cond_signal(&priv->cond);
        }
        pthread_mutex_unlock(&priv->mutex);
    }

    return avfilter_forward_command(ctx, pad_idx, target, cmd, arg, res, res_len, flags);
}

static int avtdec_output_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];

    outlink->time_base = inlink->time_base;
    if (outlink->type == AVMEDIA_TYPE_VIDEO) {
        outlink->frame_rate = inlink->frame_rate;
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
        outlink->w = inlink->w;
        outlink->h = inlink->h;
    }

    return 0;
}

static int avtdec_flush_if_need(AVFilterContext *ctx, AVCodecContext *codec_ctx)
{
    DecoderContext *priv = ctx->priv;
    AVFrame *frame;

    pthread_mutex_lock(&priv->mutex);
    if (ff_framequeue_queued_frames(&priv->in_queue)) {
        frame = ff_framequeue_peek(&priv->in_queue, 0);
        if (!frame->data[0]) {
            frame = ff_framequeue_take(&priv->in_queue);
            av_frame_free(&frame);

            while (ff_framequeue_queued_frames(&priv->out_queue)) {
                frame = ff_framequeue_take(&priv->out_queue);
                av_frame_free(&frame);
            }
            avcodec_flush_buffers(codec_ctx);
        }
    }
    pthread_mutex_unlock(&priv->mutex);

    return 0;
}

static void* avtdec_thread_loop(void *arg)
{
    AVFilterContext *ctx = arg;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    DecoderContext *priv = ctx->priv;
    AVCodecContext* codec_ctx = NULL;
    int64_t pts;
    int ret;

    ret = avtdec_open(ctx, &codec_ctx);
    if (ret < 0)
        return NULL;

    pthread_mutex_lock(&priv->mutex);
    priv->thread_exit = false;
    pthread_cond_signal(&priv->cond);
    pthread_mutex_unlock(&priv->mutex);

    while (1) {
        AVFrame *in = NULL, *out = NULL;

        /* Check whether need to exit decoding. */
        pthread_mutex_lock(&priv->mutex);
        if (priv->thread_exit) {
            pthread_mutex_unlock(&priv->mutex);
            break;
        }
        pthread_mutex_unlock(&priv->mutex);

        /* Check whether flush is needed. */
        avtdec_flush_if_need(ctx, codec_ctx);

        pthread_mutex_lock(&priv->mutex);
        if (ff_framequeue_queued_frames(&priv->out_queue) >= priv->ocnt) {
            if (!priv->thread_exit)
                pthread_cond_wait(&priv->cond, &priv->mutex);
            pthread_mutex_unlock(&priv->mutex);
            continue;
        }
        pthread_mutex_unlock(&priv->mutex);

        /* Try to get the output frame from the codec. */
        ret = avtdec_receive(ctx, codec_ctx, &out);
        if (ret >= 0 ) {
            /*
             * Put the output frame into the outqueue,
             * and mediad will send it to the downstream.
             */
            pthread_mutex_lock(&priv->mutex);
            ret = ff_framequeue_add(&priv->out_queue, out);
            pthread_mutex_unlock(&priv->mutex);

            if (ret >= 0) {
                ff_filter_set_ready(ctx, 300);
            } else {
                av_frame_free(&out);
                av_log(ctx, AV_LOG_ERROR, "Failed add decoded frame %d.\n", ret);
                break;
            }
        } else if (ret == AVERROR_EOF) {
            /*
             * Decoding is complete,
             * an empty frame is put into the inqueue
             * to inform mediad that decoding is complete.
             */
            out = av_frame_alloc();
            if (!out)
                break;

            pthread_mutex_lock(&priv->mutex);
            ret = ff_framequeue_add(&priv->out_queue, out);
            pthread_mutex_unlock(&priv->mutex);

            if (ret >= 0) {
                ff_filter_set_ready(ctx, 300);
            } else {
                av_frame_free(&out);
                av_log(ctx, AV_LOG_ERROR, "Failed add decoded frame %d.\n", ret);
                break;
            }

            avcodec_flush_buffers(codec_ctx);
        } else if (ret == AVERROR(EAGAIN)) {
            pthread_mutex_lock(&priv->mutex);
            if (ff_framequeue_queued_frames(&priv->in_queue)) {
                /* Take pkt from inqueue and send it to the decoder for decoding. */
                in = ff_framequeue_take(&priv->in_queue);
                pthread_mutex_unlock(&priv->mutex);

                ret = avtdec_send(codec_ctx, in);
                if (ret < 0) {
                    av_log(ctx, AV_LOG_ERROR, "Failed avtdec_send %d.\n", ret);
                    break;
                }
            } else {
                /* The inqueue is empty, notify mediad to put pkt into the inqueue. */
                ff_filter_set_ready(ctx, 100);
                if (!priv->thread_exit)
                    pthread_cond_wait(&priv->cond, &priv->mutex);
                pthread_mutex_unlock(&priv->mutex);
            }
        } else {
            av_log(ctx, AV_LOG_ERROR, "Failed avtdec_receive %d.\n", ret);
            break;
        }
    }

    avtdec_close(ctx, &codec_ctx);
    return NULL;
}

static int avtdec_thread_create(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    DecoderContext *priv = ctx->priv;
    struct sched_param param;
    pthread_attr_t attr;
    int ret;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, priv->stack_size);

    param.sched_priority = priv->priority;
    pthread_attr_setschedparam(&attr, &param);

    ret = pthread_create(&priv->avtdec_id, &attr, avtdec_thread_loop, ctx);
    if (ret > 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed create decoder thread %d.\n", ret);
        return AVERROR(ret);
    }

    pthread_setname_np(priv->avtdec_id, ctx->filter->name);
    return 0;
}

static int avtdec_activate(AVFilterContext* ctx)
{
    AVFilterLink* outlink = ctx->outputs[0];
    AVFilterLink* inlink = ctx->inputs[0];
    DecoderContext* priv = ctx->priv;
    AVFrame *in = NULL, *out = NULL;
    int64_t pts;
    int ret;

    /* Do not decode if sink no longer need data. */
    ret = ff_outlink_get_status(outlink);
    if (ret < 0) {
        ff_inlink_set_status(inlink, ret);
        return ret;
    }

    ff_inlink_acknowledge_status(inlink, &ret, &pts);
    if (ret == AVERROR_EOF) {
        pthread_mutex_lock(&priv->mutex);
        priv->thread_exit = true;
        pthread_cond_signal(&priv->cond);
        pthread_mutex_unlock(&priv->mutex);

        /* Wait for the decoding thread to exit. */
        ret = pthread_join(priv->avtdec_id, NULL);
        if (ret != 0)
            return AVERROR(ret);

        priv->avtdec_id = -1;
        ff_avfilter_link_set_in_status(outlink, AVERROR_EOF, AV_NOPTS_VALUE);
        return ret;
    }

    if (priv->avtdec_id < 0) {
        if (ff_inlink_consume_frame(inlink, &in) <= 0) {
            ff_inlink_request_frame(inlink);
            return AVERROR(EAGAIN);
        }

        ff_framequeue_init(&priv->in_queue, NULL);
        ff_framequeue_init(&priv->out_queue, NULL);

        /* Before creating the vtdec thread, make sure inqueue has 1 pkt to create the decoder. */
        ff_framequeue_add(&priv->in_queue, in);

        ret = avtdec_thread_create(ctx);
        if (ret < 0)
            return ret;

        /* Wait for the decoder to initialize. */
        pthread_mutex_lock(&priv->mutex);
        while (priv->thread_exit)
            pthread_cond_wait(&priv->cond, &priv->mutex);
        pthread_mutex_unlock(&priv->mutex);

        outlink->frame_wanted_out = 1;
        return 0;
    }

    pthread_mutex_lock(&priv->mutex);
    if (!ff_outlink_frame_wanted(outlink) && ff_framequeue_queued_frames(&priv->out_queue) >= priv->ocnt) {
        /* The downstream does not require frame,
         * but the outqueue has stored ocnt frames,
         * no more production, return directly.
         */
        pthread_mutex_unlock(&priv->mutex);
        return 0;
    } else if (ff_outlink_frame_wanted(outlink) && ff_framequeue_queued_frames(&priv->out_queue)) {
        /* The downstream require a frame,
         * and the outqueue has a frame,
         * so the frame is sent from the outqueue to the outlink.
         */
        out = ff_framequeue_take(&priv->out_queue);
        pthread_cond_signal(&priv->cond);

        if (out->data[0]) {
            ff_filter_frame(outlink, out);
        } else {
            /* Empty frame received, decoding completed */
            av_frame_free(&out);
            pthread_mutex_unlock(&priv->mutex);
            return avfilter_forward_command(ctx, 0, NULL, "completed", NULL, NULL, 0, AVFILTER_CMD_FLAG_REVERSE);
        }
    }

    /* Try to put the inlink pkt into the inqueue */
    if (ff_framequeue_queued_frames(&priv->in_queue) < priv->icnt) {
        if (ff_inlink_consume_frame(inlink, &in) <= 0)
            ff_inlink_request_frame(inlink);
        else {
            ff_framequeue_add(&priv->in_queue, in);
            pthread_cond_signal(&priv->cond);
        }
    }

    pthread_mutex_unlock(&priv->mutex);
    return ret;
}

#if CONFIG_ATDEC_FILTER
static const AVFilterPad atdec_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
};

static const AVFilterPad atdec_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = avtdec_output_props,
    },
};

static const AVClass atdec_class = {
    .class_name = "atdec_class",
    .item_name  = av_default_item_name,
    .option     = avtdec_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_af_atdec = {
    .name            = "atdec",
    .description     = NULL_IF_CONFIG_SMALL("Audio thread decoder filter."),
    .priv_size       = sizeof(DecoderContext),
    .priv_class      = &atdec_class,
    .init            = avtdec_init,
    .uninit          = avtdec_uninit,
    .activate        = avtdec_activate,
    .flags           = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    FILTER_QUERY_FUNC(avtdec_query_formats),
    FILTER_INPUTS(atdec_inputs),
    FILTER_OUTPUTS(atdec_outputs),
    .forward_command = avtdec_forward_command,
};
#endif

#if CONFIG_VTDEC_FILTER
static const AVFilterPad vtdec_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad vtdec_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = avtdec_output_props,
    },
};

static const AVClass vtdec_class = {
    .class_name = "vtdec_class",
    .item_name  = av_default_item_name,
    .option     = avtdec_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_vf_vtdec = {
    .name            = "vtdec",
    .description     = NULL_IF_CONFIG_SMALL("Video thread decoder filter."),
    .priv_size       = sizeof(DecoderContext),
    .priv_class      = &vtdec_class,
    .init            = avtdec_init,
    .uninit          = avtdec_uninit,
    .activate        = avtdec_activate,
    .flags           = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    FILTER_QUERY_FUNC(avtdec_query_formats),
    FILTER_INPUTS(vtdec_inputs),
    FILTER_OUTPUTS(vtdec_outputs),
    .forward_command = avtdec_forward_command,
};
#endif
