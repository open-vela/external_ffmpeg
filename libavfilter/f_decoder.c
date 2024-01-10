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

#include "libavcodec/avcodec.h"
#include "packet_wrapper.h"
#include "filters.h"

typedef struct DecoderContext {
    const AVClass  *class;
    AVCodecContext *codec_ctx;
    int64_t        next_pts;
} DecoderContext;

static int decoder_open(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    DecoderContext *priv = ctx->priv;
    AVCodecParameters *param = NULL;
    AVFrame *frame = NULL;
    const AVCodec *codec = NULL;
    int ret;

    if (!ff_inlink_check_available_frame(inlink))
        return AVERROR(EAGAIN);

    frame = ff_inlink_peek_frame(inlink, 0);
    if (!frame || !frame->opaque_ref) {
        av_log(ctx, AV_LOG_INFO, "DEBUG: %s Lack negotiation parameter.\n", __func__);
        return AVERROR(EINVAL);
    }

    unwrap_frame(frame, NULL, &param);
    codec = avcodec_find_decoder(param->codec_id);
    if (!codec) {
        av_log(ctx, AV_LOG_INFO, "DEBUG: %s Failed find codec.\n", __func__);
        return AVERROR(EINVAL);
    }

    priv->codec_ctx = avcodec_alloc_context3(codec);
    if (!priv->codec_ctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(priv->codec_ctx, param);
    if (ret < 0)
        goto out;

    priv->codec_ctx->thread_count = ff_filter_get_nb_threads(ctx);
    ret = avcodec_open2(priv->codec_ctx, codec, &frame->metadata);
    if (ret < 0) {
        av_log(ctx, AV_LOG_INFO, "DEBUG: %s Failed open codec %d %s.\n", __func__, ret, av_err2str(ret));
        goto out;
    }

    priv->next_pts = AV_NOPTS_VALUE;
    return 0;

out:
    avcodec_free_context(&priv->codec_ctx);
    return ret;
}

static void decoder_close(AVFilterContext *ctx)
{
    DecoderContext *priv = ctx->priv;

    if (!priv->codec_ctx)
        return;

    avcodec_close(priv->codec_ctx);
    avcodec_free_context(&priv->codec_ctx);
}

static int decoder_send(AVFilterContext *ctx, AVFrame *in)
{
    DecoderContext *priv = ctx->priv;
    AVPacket *pkt;
    int ret;

    unwrap_frame(in, &pkt, NULL);
    ret = avcodec_send_packet(priv->codec_ctx, pkt);
    av_frame_free(&in);
    if (ret < 0) {
        /* ignore AVERROR_INVALIDDATA, and try again */
        if (ret == AVERROR_INVALIDDATA)
            ret = AVERROR(EAGAIN);
    }

    return ret;
}

static int decoder_receive(AVFilterContext *ctx, AVFrame **out)
{
    DecoderContext *priv = ctx->priv;
    AVFrame *frame;
    int ret;

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    ret = avcodec_receive_frame(priv->codec_ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    if (priv->codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (frame->pts == AV_NOPTS_VALUE && priv->next_pts != AV_NOPTS_VALUE)
            frame->pts = priv->next_pts;

        if (frame->pts != AV_NOPTS_VALUE)
            priv->next_pts = frame->pts + frame->nb_samples;
    }

    frame->time_base = ctx->inputs[0]->time_base;
    *out = frame;
    return 0;
}

static int decoder_activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *in  = NULL, *out = NULL;
    DecoderContext *priv = ctx->priv;
    int64_t pts;
    int ret;

    ret = ff_outlink_get_status(outlink);
    if (ret < 0) {
        if (ret == AVERROR_EOF)
            decoder_close(ctx);

        return ret;
    }

    if (!priv->codec_ctx) {
        ret = decoder_open(ctx);
        if (ret < 0)
            return ret;
    }

    while (1) {
        ret = decoder_receive(ctx, &out);
        if (ret >= 0) {
            ret = ff_filter_frame(outlink, out);
            break;
        } else if (ret != AVERROR(EAGAIN))
            break;

        ret = ff_inlink_consume_frame(inlink, &in);
        if (ret <= 0) {
            ff_inlink_request_frame(inlink);
            break;
        }

        ret = decoder_send(ctx, in);
        if (ret < 0)
            break;
    }

    ff_inlink_acknowledge_status(inlink, &ret, &pts);
    if (ret == AVERROR_EOF) {
        decoder_close(ctx);
        ff_avfilter_link_set_in_status(outlink, AVERROR_EOF, AV_NOPTS_VALUE);
    }

    return ret;
}

static int decoder_query_formats(AVFilterContext *ctx)
{
    AVFilterLink *inlink  = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterFormats *codecs = NULL;
    const AVCodec *codec = NULL;
    void *iterate = NULL;
    enum AVCodecID id;
    int ret, dup = 0;

    /* Input uses compress, output uses raw. */
    while (codec = av_codec_iterate(&iterate)) {
        if (codec->type != inlink->type || av_codec_is_encoder(codec))
            continue;

        id = codec->id;
        if (avcodec_is_pcm_lossless(id)) {
            if (!dup) {
                id = AV_CODEC_ID_RAWAUDIO;
                dup = 1;
            } else {
                continue;
            }
        }

        if (ret = ff_add_format(&codecs, id) < 0)
            goto out;
    }

    if ((ret = ff_formats_ref(codecs, &inlink->outcfg.codecs)) < 0)
        goto out;

    if ((ret = ff_formats_ref(ff_all_raw_codecs(outlink->type), &outlink->incfg.codecs)) < 0)
        goto out;

    /* Formats, samplerates, channel layouts should simply align with source. */
    if ((ret = ff_set_common_formats(ctx, inlink->incfg.formats)) < 0)
        goto out;

    if (inlink->type == AVMEDIA_TYPE_AUDIO) {
        if ((ret = ff_set_common_samplerates(ctx, inlink->incfg.samplerates)) < 0)
            goto out;

        if ((ret = ff_set_common_channel_layouts(ctx, inlink->incfg.channel_layouts)) < 0)
            goto out;
    }

    return 0;

out:
    ff_formats_unref(&codecs);

    return ret;
}

static int decoder_process_command(AVFilterContext *ctx,
                                   const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    DecoderContext* priv = ctx->priv;

    if (priv->codec_ctx && !strcmp(cmd, "flush")) {
        avcodec_flush_buffers(priv->codec_ctx);
        return 0;
    }

    return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
}

static int decoder_forward_command(AVFilterContext *ctx,
                                   int pad_idx, const char *target, const char *cmd,
                                   const char *arg, char *res, int res_len, int flags)
{
    int ret;

    ret = decoder_process_command(ctx, cmd, arg, res, res_len, flags);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed dec:%s forward_command %s %d,%s\n",
               ctx->filter->name, cmd, ret, av_err2str(ret));
        return ret;
    }

    return avfilter_forward_command(ctx, pad_idx, target, cmd, arg, res, res_len, flags);
}

static int decoder_output_props(AVFilterLink *outlink)
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

#if CONFIG_ADECODER_FILTER
static const AVFilterPad avfilter_af_adecoder_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
};

static const AVFilterPad avfilter_af_adecoder_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
        .config_props = decoder_output_props,
    },
};

static const AVClass adecoder_class = {
    .class_name = "adecoder_class",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_af_adecoder = {
    .name        = "adecoder",
    .description = NULL_IF_CONFIG_SMALL("audio decoder filter."),
    .priv_size   = sizeof(DecoderContext),
    .priv_class  = &adecoder_class,
    .activate    = decoder_activate,
    .flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    FILTER_QUERY_FUNC(decoder_query_formats),
    FILTER_INPUTS(avfilter_af_adecoder_inputs),
    FILTER_OUTPUTS(avfilter_af_adecoder_outputs),
    .process_command = decoder_process_command,
    .forward_command = decoder_forward_command,
};
#endif

#if CONFIG_DECODER_FILTER
static const AVFilterPad avfilter_vf_decoder_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad avfilter_vf_decoder_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props = decoder_output_props,
    },
};

static const AVClass vdecoder_class = {
    .class_name = "vdecoder_class",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_vf_decoder = {
    .name        = "decoder",
    .description = NULL_IF_CONFIG_SMALL("video decoder filter."),
    .priv_size   = sizeof(DecoderContext),
    .priv_class  = &vdecoder_class,
    .activate    = decoder_activate,
    .flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    FILTER_QUERY_FUNC(decoder_query_formats),
    FILTER_INPUTS(avfilter_vf_decoder_inputs),
    FILTER_OUTPUTS(avfilter_vf_decoder_outputs),
    .process_command = decoder_process_command,
    .forward_command = decoder_forward_command,
};
#endif
