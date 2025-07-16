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
 * audio tinycompress sink
 */

#include <libavutil/avstring.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
#include <sound/compress_params.h>
#include <tinycompress/tinycompress.h>
#include <poll.h>

#include "amix.h"
#include "filters.h"
#include "avfilter.h"
#include "formats.h"

typedef struct CompSinkPriv {
    const AVClass *class;
    AVCodecContext *enc_ctx;
    int nb_inputs;
    char *devname;
    int sample_fmt;
    uint32_t sample_rate;
    AVChannelLayout ch_layout;
    enum AVCodecID codec_id;
    AVPacket *last_pkt;
    AMixContext *mix;
    FAR struct compress *compress;
} CompSinkPriv;

static int tinycomprsink_subfmt_to_avcodec(int subfmt)
{
    switch (subfmt) {
        case AUDIO_FMT_PCM:
            return AV_CODEC_ID_PCM_S16LE;
        case AUDIO_FMT_MP3:
            return AV_CODEC_ID_MP3;
        case AUDIO_FMT_SBC:
            return AV_CODEC_ID_SBC;
        case AUDIO_FMT_AAC:
            return AV_CODEC_ID_AAC;
        case AUDIO_FMT_WAV:
            return AV_CODEC_ID_WAVARC;
        case AUDIO_FMT_OPUS:
            return AV_CODEC_ID_OPUS;
    }

    return AV_CODEC_ID_NONE;
}

static int tinycomprsink_subfmt_to_smpfmt(int subfmt)
{
    switch (subfmt) {
        case AUDIO_SUBFMT_PCM_S8:
        case AUDIO_SUBFMT_PCM_U8:
            return AV_SAMPLE_FMT_U8;
        case AUDIO_SUBFMT_PCM_S16_LE:
        case AUDIO_SUBFMT_PCM_S16_BE:
            return AV_SAMPLE_FMT_S16;
        case AUDIO_SUBFMT_PCM_S32_LE:
        case AUDIO_SUBFMT_PCM_S32_BE:
            return AV_SAMPLE_FMT_S32;
        case AUDIO_SUBFMT_PCM_MP3   :
            return AV_SAMPLE_FMT_S16P;
    }

    return AV_SAMPLE_FMT_NONE;
}

static int tinycomprsink_open_encoder(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    const AVCodec *enc;
    int ret;

    enc = avcodec_find_encoder(priv->codec_id);
    if (!enc)
        return AVERROR(EINVAL);

    priv->enc_ctx = avcodec_alloc_context3(enc);
    if (!priv->enc_ctx)
        return AVERROR(ENOMEM);

    priv->enc_ctx->codec_type  = AVMEDIA_TYPE_AUDIO;
    priv->enc_ctx->sample_fmt  = priv->sample_fmt;
    priv->enc_ctx->sample_rate = priv->sample_rate;
    av_channel_layout_copy(&priv->enc_ctx->ch_layout, &priv->ch_layout);

    ret = avcodec_open2(priv->enc_ctx, enc, NULL);
    if (ret < 0) {
        avcodec_free_context(&priv->enc_ctx);
        return ret;
    }

    return 0;
}

static void tinycomprsink_close_encoder(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;

    if (!priv->enc_ctx)
        return;

    avcodec_free_context(&priv->enc_ctx);
}

static int tinycomprsink_start(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    struct compr_config config = {0};
    int ret;

    if (priv->compress)
        return 0;

    priv->compress = compress_open_by_name(priv->devname, COMPRESS_IN, &config);
    av_channel_layout_default(&priv->ch_layout, config.codec->ch_in);
    priv->sample_fmt = tinycomprsink_subfmt_to_smpfmt(config.codec->format);
    priv->sample_rate = config.codec->sample_rate;
    priv->codec_id = tinycomprsink_subfmt_to_avcodec(config.codec->id);
    if (config.codec)
        free(config.codec);

    if (!priv->mix) {
        priv->mix = ff_amix_alloc(priv->sample_rate, priv->sample_fmt, priv->ch_layout.nb_channels);
        if (!priv->mix)
            return AVERROR(ENOMEM);
    }

    compress_nonblock(priv->compress, 1);
    priv->last_pkt = av_packet_alloc();
    if (!priv->last_pkt) {
        ret = -ENOMEM;
        goto out;
    }

    ret = tinycomprsink_open_encoder(ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "tinycomprsink fail to open encoder\n");
        goto out;
    }

    if (priv->enc_ctx->frame_size)
        ff_amix_set_frame_size(priv->mix, priv->enc_ctx->frame_size);

    ret = compress_start(priv->compress);
    if (ret < 0)
        goto out;

    return 0;

out:
    avcodec_free_context(&priv->enc_ctx);
    av_packet_free(&priv->last_pkt);
    if (priv->compress) {
        compress_close(priv->compress);
        priv->compress = NULL;
    }
    if (priv->mix) {
        ff_amix_free(priv->mix);
        priv->mix = NULL;
    }

    return ret;
}

static void tinycomprsink_stop(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;

    if (!priv->compress)
        return;

    av_packet_free(&priv->last_pkt);
    tinycomprsink_close_encoder(ctx);
    compress_drain(priv->compress);
    compress_close(priv->compress);
    priv->compress = NULL;
}

static int tinycomprsink_output_packet(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    AVPacket *pkt = priv->last_pkt;
    int ret = 0;

    if (!priv->compress)
        return 0;

    while (pkt) {
        if (pkt->data) {
            ret = compress_write(priv->compress, pkt->data, pkt->size);
            if (ret == pkt->size)
                av_packet_unref(pkt);
            else if (ret > 0) {
                pkt->data += ret;
                pkt->size -= ret;
                break;
            } else
                break;
        }

        ret = avcodec_receive_packet(priv->enc_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                ret = 0;
            break;
        }
    }

    if (ret == AVERROR_EOF)
        tinycomprsink_stop(ctx);

    return ret;
}

static int tinycomprsink_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    CompSinkPriv *priv = ctx->priv;
    int ret;

    ret = avcodec_send_frame(priv->enc_ctx, frame);
    if (ret < 0)
        return ret;

    return tinycomprsink_output_packet(ctx);
}

static int tinycomprsink_activate(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    int i, ret, empty_inputs = 0;
    AVFrame *frame = NULL;
    AVFilterLink *link;
    int64_t pts;

    ret = tinycomprsink_output_packet(ctx);
    if (ret < 0)
        return ret;

    for (i = 0; i < priv->nb_inputs; i++) {
        link = ctx->inputs[i];

        if (ff_inlink_check_available_frame(link)) {
            ret = tinycomprsink_start(ctx);
            if (ret < 0)
                return ret;

            ret = ff_amix_input_write(priv->mix, link);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "input[%d] write to mix failed, ret:%d.\n", i, ret);
                continue;
            }
        }
    }

    ff_amix_read(priv->mix, &frame);
    if (frame) {
        ret = tinycomprsink_send_frame(ctx, frame);
        av_frame_free(&frame);
        if (ret >= 0)
            ff_filter_set_ready(ctx, 100);
        return ret;
    }

    for (i = 0; i < priv->nb_inputs; i++) {
        link = ctx->inputs[i];

        ff_inlink_acknowledge_status(link, &ret, &pts);
        if (ret >= 0) {
            if (ff_amix_input_want(priv->mix, link))
                ff_inlink_request_frame(link);
        } else if (ret == AVERROR_EOF) {
            if (!ff_amix_input_empty(priv->mix, link))
                ff_filter_set_ready(ctx, 100);
            else
                empty_inputs++;
        }
    }

    if (empty_inputs == priv->nb_inputs) { /* notify encoder there is no more data to handle */
        if (priv->mix) {
            ff_amix_free(priv->mix);
            priv->mix = NULL;
        }
        return tinycomprsink_send_frame(ctx, NULL);
    }

    return 0;
}

static int tinycomprsink_process_command(AVFilterContext *ctx,
                                         const char *cmd, const char *args,
                                         char *res, int res_len, int flags)
{
    CompSinkPriv *priv = ctx->priv;

    if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll_fd = (struct pollfd *)res;
        if(priv->compress) {
            poll_fd[0].fd = compress_get_file_descriptor(priv->compress);
            poll_fd[0].events = POLLIN;
            return 1;
        }
        return AVERROR(EINVAL);
    } else if (!strcmp(cmd, "poll_available")) {
        if (priv->compress) {
            compress_poll_available(priv->compress);
            ff_filter_set_ready(ctx, 100);
        }
        return 0;
    }

    return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
}

static int tinycomprsink_init(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
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

    return 0;
}

#define OFFSET(x) offsetof(CompSinkPriv, x)
#define FLAGS  AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define FLAGSR FLAGS|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption tinycomprsink_options[] = {
    { "inputs",      "", OFFSET(nb_inputs),   AV_OPT_TYPE_INT,    {.i64 = 1},       1, INT16_MAX, FLAGS },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(tinycomprsink);

const AVFilter ff_asink_tinycomprsink = {
    .name            = "tinycomprsink",
    .description     = NULL_IF_CONFIG_SMALL("Audio tinycompress sink"),
    .priv_class      = &tinycomprsink_class,
    .priv_size       = sizeof(CompSinkPriv),
    .init            = tinycomprsink_init,
    .activate        = tinycomprsink_activate,
    .process_command = tinycomprsink_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL | AVFILTER_FLAG_DYNAMIC_INPUTS,
};
