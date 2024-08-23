/*
 * libhelix aac decoder
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
 * libhelix AAC decoder implementation
 */

#include "avcodec.h"
#include "internal.h"
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"
#include "mpeg4audio.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include <aacdec.h>

#define SYNCWORDH 0xff
#define SYNCWORDL 0xf0
#define LIBHELIX_AAC_MAX_CHANNELS 2
#define LIBHELIX_AAC_MAX_NSAMPS   2048

#define LATM_SYNCWORDH 0xe0
#define LATM_SYNCWORDL 0x56

typedef struct HAACDecContext {
    AVClass *class;
    HAACDecoder context;
    uint8_t *pcm;
    int pcm_size;
    MPEG4AudioConfig m4ac;
} HAACDecContext;

static int aac_decode_init(AVCodecContext *avctx)
{
    HAACDecContext *aac = avctx->priv_data;
    GetBitContext gb;
    int ret;

    aac->context = AACInitDecoder();
    if (!aac->context)
        return AVERROR(ENOMEM);


    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    aac->pcm_size = LIBHELIX_AAC_MAX_NSAMPS * LIBHELIX_AAC_MAX_CHANNELS *
                    av_get_bytes_per_sample(avctx->sample_fmt);

    aac->pcm = av_malloc(aac->pcm_size);
    if (!aac->pcm)
        return AVERROR(ENOMEM);

    if (avctx->extradata_size > 0) {
        if ((ret = init_get_bits(&gb, avctx->extradata, avctx->extradata_size)) < 0)
            return ret;

        if ((ret = ff_mpeg4audio_get_config_gb(&aac->m4ac, &gb, 1, NULL)) < 0)
            return ret;

    }

    return 0;
}

static int aac_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame_ptr, AVPacket *avpkt)
{
    HAACDecContext *aac = avctx->priv_data;
    AACFrameInfo info;
    uint8_t *in_data;
    int ret, in_size;

    if (!aac)
        return AVERROR(EIO);

    in_data = avpkt->data;
    in_size = avpkt->size;

    if ((in_data[0] & SYNCWORDH) == SYNCWORDH && (in_data[1] & SYNCWORDL) == SYNCWORDL) {
        ret = AACSetFormat(aac->context, AAC_FF_ADTS);
        if (ret < 0)
            return 0;
    } else {
        info.sampRateCore = aac->m4ac.sample_rate ? aac->m4ac.sample_rate : avctx->sample_rate;
        info.nChans  = aac->m4ac.channels ? aac->m4ac.channels : avctx->ch_layout.nb_channels;
        info.profile = AAC_PROFILE_LC;
        ret = AACSetRawBlockParams(aac->context, 0, &info);
        if (ret < 0)
            return ret;
    }

    ret = AACDecode(aac->context, &in_data, &in_size, (int16_t *)aac->pcm);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "%s error ret %d.\n", __func__, ret);
        AACFlushCodec(aac->context);
        return ret;
    }

    AACGetLastFrameInfo(aac->context, &info);

    if (!avctx->sample_rate)
         avctx->sample_rate = info.sampRateOut;

    if (!avctx->ch_layout.nb_channels)
         av_channel_layout_default(&avctx->ch_layout, info.nChans);

    avctx->frame_size = info.outputSamps / info.nChans;

    frame->nb_samples = avctx->frame_size;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    memcpy(frame->extended_data[0], aac->pcm,
           avctx->ch_layout.nb_channels * avctx->frame_size *
           av_get_bytes_per_sample(avctx->sample_fmt));

    *got_frame_ptr = 1;

    return avpkt->size - in_size;
}

static int aac_latm_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                                 int *got_frame_ptr, AVPacket *avpkt)
{
    HAACDecContext *aac = avctx->priv_data;
    AACFrameInfo info;
    uint8_t *in_data;
    int ret, in_size;

    if (!aac)
        return AVERROR(EIO);

    in_data = avpkt->data;
    in_size = avpkt->size;

    if(in_data[0] != LATM_SYNCWORDL || (in_data[1] & 0xf0) != LATM_SYNCWORDH) {
        av_log(avctx, AV_LOG_ERROR, "%s Invalid sync word! %0x %0x\n",
               __func__, in_data[0], (in_data[1] & 0xf0));
        return AVERROR_INVALIDDATA;
    }

    AACSetFormat(aac->context, AAC_FF_LATM_MCP1);

    in_data += 3;
    in_size -= 3;
    ret = AACDecode(aac->context, &in_data, &in_size, (int16_t *)aac->pcm);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "%s error ret %d.\n", __func__, ret);
        AACFlushCodec(aac->context);
        return ret;
    }

    AACGetLastFrameInfo(aac->context, &info);

    if (!avctx->sample_rate)
         avctx->sample_rate = info.sampRateOut;

    if (!avctx->ch_layout.nb_channels)
         av_channel_layout_default(&avctx->ch_layout, info.nChans);

    avctx->frame_size = info.outputSamps / info.nChans;

    frame->nb_samples = avctx->frame_size;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    memcpy(frame->extended_data[0], aac->pcm,
           avctx->ch_layout.nb_channels * avctx->frame_size *
           av_get_bytes_per_sample(avctx->sample_fmt));

    *got_frame_ptr = 1;

    return avpkt->size;
}

static av_cold int aac_decode_close(AVCodecContext *avctx)
{
    HAACDecContext *aac = avctx->priv_data;

    if (aac->context)
        AACFreeDecoder(aac->context);
    av_freep(&aac->pcm);

    return 0;
}

const FFCodec ff_libhelix_aac_decoder = {
    .p.name            = "libhelix_aac",
    .p.long_name       = NULL_IF_CONFIG_SMALL("libHelix AAC Decoder"),
    .p.type            = AVMEDIA_TYPE_AUDIO,
    .p.id              = AV_CODEC_ID_AAC,
    .priv_data_size    = sizeof(HAACDecContext),
    .init              = aac_decode_init,
    FF_CODEC_DECODE_CB(aac_decode_frame),
    .close             = aac_decode_close,
    .p.capabilities    = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .p.sample_fmts     = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                         AV_SAMPLE_FMT_NONE },
    .p.ch_layouts      = (const AVChannelLayout[]) { AV_CHANNEL_LAYOUT_MONO,
                                                     AV_CHANNEL_LAYOUT_STEREO, { 0 } },
};

const FFCodec ff_libhelix_aac_latm_a2dp_decoder = {
    .p.name            = "libhelix_aac_latm_a2dp",
    .p.long_name       = NULL_IF_CONFIG_SMALL("libHelix AAC Decoder"),
    .p.type            = AVMEDIA_TYPE_AUDIO,
    .p.id              = AV_CODEC_ID_AAC_LATM_A2DP,
    .priv_data_size    = sizeof(HAACDecContext),
    .init              = aac_decode_init,
    FF_CODEC_DECODE_CB(aac_latm_decode_frame),
    .close             = aac_decode_close,
    .p.capabilities    = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .p.sample_fmts     = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                         AV_SAMPLE_FMT_NONE },
    .p.ch_layouts      = (const AVChannelLayout[]) { AV_CHANNEL_LAYOUT_MONO,
                                                     AV_CHANNEL_LAYOUT_STEREO, { 0 } },
    .bsfs              = "a2dp_rechunk",
};
