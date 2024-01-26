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

#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"

#include "filters.h"
#include "packet_wrapper.h"

typedef struct EncoderContext {
    const AVClass       *class;
    AVCodecContext      *codec_ctx;
    enum AVCodecID      codec_id;
    bool                started;
} EncoderContext;

static int encoder_open(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterLink *inlink = ctx->inputs[0];
    EncoderContext *priv = ctx->priv;
    const AVCodec *codec = NULL;
    int ret = AVERROR(EINVAL);
    AVDictionary *dict = NULL;

    codec = avcodec_find_encoder(priv->codec_id);
    if (!codec) {
        av_log(ctx, AV_LOG_ERROR, "Encoder id %d not found\n", priv->codec_id);
        return AVERROR(EINVAL);
    }

    priv->codec_ctx = avcodec_alloc_context3(codec);
    if (!priv->codec_ctx)
        return AVERROR(ENOMEM);

    if (inlink->type == AVMEDIA_TYPE_AUDIO) {
        priv->codec_ctx->sample_fmt  = inlink->format;
        priv->codec_ctx->sample_rate = inlink->sample_rate;
        priv->codec_ctx->ch_layout   = inlink->ch_layout;
        priv->codec_ctx->time_base   = (AVRational){ 1, inlink->sample_rate };
    } else {
        priv->codec_ctx->pix_fmt   = inlink->format;
        priv->codec_ctx->width     = inlink->w;
        priv->codec_ctx->height    = inlink->h;
        priv->codec_ctx->time_base = inlink->time_base;
        priv->codec_ctx->framerate = inlink->frame_rate;
    }

    /**
     * behavior, e.g. allow mjpeg encoder to process images with limited color range.
     *
     * Note: set strict_std_compliance to FF_COMPLIANCE_UNOFFICIAL does not mean the
     * generated *.mp4 or *.jpg file is non-standard, but indicates that the encoder
     * is allowed to process non-standard input data.
     */

    priv->codec_ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;

    /* Some codec context configurations cannot be passed through link. */

    ret = avfilter_process_command(outlink->dst, "get_options", NULL,
                                   (char*)&dict, sizeof(AVDictionary **), 0);
    if (ret < 0)
        goto err;

    ret = avcodec_open2(priv->codec_ctx, codec, &dict);
    av_dict_free(&dict);
    if (ret < 0)
        goto err;

    return 0;

err:
    avcodec_free_context(&priv->codec_ctx);
    return ret;
}

static void encoder_close(AVFilterContext *ctx)
{
    EncoderContext *priv = ctx->priv;

    avcodec_close(priv->codec_ctx);
    avcodec_free_context(&priv->codec_ctx);
    priv->started = false;
}

static int encoder_encode(AVFilterContext *ctx, AVFrame *in, AVFrame **out)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVCodecParameters *params = NULL;
    EncoderContext *priv = ctx->priv;
    AVPacket *pkt = NULL;
    AVFrame *frame;
    int ret;

    ret = avcodec_send_frame(priv->codec_ctx, in);
    av_frame_free(&in);
    if (ret < 0)
        return ret;

    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);

    ret = avcodec_receive_packet(priv->codec_ctx, pkt);
    if (ret < 0)
        goto err;

    if (!priv->started) {
        params = avcodec_parameters_alloc();
        if (!params)
            goto err;

        /* Attach parameters to first frame, to initialize stream in muxer. */
        ret = avcodec_parameters_from_context(params, priv->codec_ctx);
        if (ret < 0)
            goto err;

        priv->started = true;
    }

    frame = wrap_frame(pkt, params, NULL);
    if (!frame) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    frame->format          = inlink->format;
    if (inlink->type == AVMEDIA_TYPE_VIDEO) {
        frame->width       = priv->codec_ctx->width;
        frame->height      = priv->codec_ctx->height;
    } else {
        frame->nb_samples  = priv->codec_ctx->frame_size;
        frame->sample_rate = priv->codec_ctx->sample_rate;
        av_channel_layout_copy(&frame->ch_layout, &priv->codec_ctx->ch_layout);
    }

    *out = frame;
    return 0;

err:
    avcodec_parameters_free(&params);
    av_packet_free(&pkt);
    return ret;
}

static int encoder_activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterLink *inlink = ctx->inputs[0];
    EncoderContext *priv = ctx->priv;
    int frame_size, ret;
    AVFrame *in  = NULL;
    AVFrame *out = NULL;
    int64_t pts;

    ff_inlink_acknowledge_status(inlink, &ret, &pts);
    if (ret == AVERROR_EOF) {
        ff_avfilter_link_set_in_status(outlink, AVERROR_EOF, AV_NOPTS_VALUE);
        if (priv->codec_ctx) {
            ret = encoder_encode(ctx, NULL, &out);
            encoder_close(ctx);
        }

    } else if ((ret = ff_outlink_get_status(outlink)) < 0) {
        ff_inlink_set_status(inlink, ret);
        if (priv->codec_ctx) {
            ret = encoder_encode(ctx, NULL, &out);
            encoder_close(ctx);
        }

    } else if (ff_inlink_check_available_frame(inlink)) {
        if (!priv->codec_ctx) {
            ret = encoder_open(ctx);
            if (ret < 0)
                return ret;
        }

        frame_size = priv->codec_ctx->frame_size;
        if (frame_size)
            ret = ff_inlink_consume_samples(inlink, frame_size, frame_size, &in);
        else
            ret = ff_inlink_consume_frame(inlink, &in);
        if (ret < 0)
            return ret;

        if (ret > 0) {
            /* Support gop config for video encoding */
            in->pict_type = AV_PICTURE_TYPE_NONE;

            ret = encoder_encode(ctx, in, &out);
        }
    }

    if (ret == 0 || ret == AVERROR(EAGAIN))
        ff_inlink_request_frame(inlink);
    if (out)
        ret = ff_filter_frame(outlink, out);

    return ret;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    AVFilterLink *inlink  = ctx->inputs[0];
    int ret;

    /**
     * Auto encoder would be inserted to link, whose soucre filter and sink
     * filter are all been queried, so we can simply follows formats of
     * sink filter to ensure the negotiation.
     */

    if ((ret = ff_formats_ref(outlink->outcfg.codecs, &outlink->incfg.codecs)) < 0)
        return ret;

    if ((ret = ff_formats_ref(ff_all_raw_codecs(inlink->type), &inlink->outcfg.codecs)) < 0)
        return ret;


    if ((ret = ff_set_common_formats(ctx, outlink->outcfg.formats)) < 0)
        return ret;

    if (inlink->type == AVMEDIA_TYPE_AUDIO) {
        if ((ret = ff_set_common_samplerates(ctx, outlink->outcfg.samplerates)) < 0)
            return ret;

        if ((ret = ff_set_common_channel_layouts(ctx, outlink->outcfg.channel_layouts)) < 0)
            return ret;
    }

    return 0;
}

static int encoder_output_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    EncoderContext *priv = ctx->priv;

    if (outlink->type == AVMEDIA_TYPE_AUDIO && outlink->codec == AV_CODEC_ID_RAWAUDIO)
        priv->codec_id = av_get_pcm_codec(outlink->format, -1);
    else
        priv->codec_id = outlink->codec;

    outlink->time_base = inlink->time_base;
    if (outlink->type == AVMEDIA_TYPE_VIDEO) {
        outlink->frame_rate = inlink->frame_rate;
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
        outlink->w = inlink->w;
        outlink->h = inlink->h;
    }

    return 0;
}

#if CONFIG_AENCODER_FILTER
static const AVFilterPad avfilter_af_aencoder_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
    },
};

static const AVFilterPad avfilter_af_aencoder_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = encoder_output_props,
    },
};

static const AVClass aencoder_class = {
    .class_name       = "aencoder_class",
    .item_name        = av_default_item_name,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_af_aencoder = {
    .name             = "aencoder",
    .description      = NULL_IF_CONFIG_SMALL("audio encode filter."),
    .priv_size        = sizeof(EncoderContext),
    .priv_class       = &aencoder_class,
    .activate         = encoder_activate,
    .flags            = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    FILTER_QUERY_FUNC(query_formats),
    FILTER_INPUTS(avfilter_af_aencoder_inputs),
    FILTER_OUTPUTS(avfilter_af_aencoder_outputs),
};
#endif

#if CONFIG_ENCODER_FILTER
static const AVFilterPad avfilter_vf_encoder_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad avfilter_vf_encoder_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = encoder_output_props,
    },
};

static const AVClass vencoder_class = {
    .class_name       = "vencoder_class",
    .item_name        = av_default_item_name,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_vf_encoder = {
    .name             = "encoder",
    .description      = NULL_IF_CONFIG_SMALL("video encode filter."),
    .priv_size        = sizeof(EncoderContext),
    .priv_class       = &vencoder_class,
    .activate         = encoder_activate,
    .flags            = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    FILTER_QUERY_FUNC(query_formats),
    FILTER_INPUTS(avfilter_vf_encoder_inputs),
    FILTER_OUTPUTS(avfilter_vf_encoder_outputs),
};
#endif /* CONFIG_ENCODER_FILTER */
