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
#include "libavutil/intreadwrite.h"

#include <aacdec.h>

#define SYNCWORDH 0xff
#define SYNCWORDL 0xf0

#define LIBHELIX_AAC_MAX_CHANNELS 2
#define LIBHELIX_AAC_MAX_NSAMPS   2048

typedef struct HAACDecContext {
    AVClass *class;
    HAACDecoder context;
    uint8_t *pcm;
    int pcm_size;
} HAACDecContext;

static int aac_decode_init(AVCodecContext *avctx)
{
    HAACDecContext *aac = avctx->priv_data;
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

    return 0;
}

static int aac_decode_frame(AVCodecContext *avctx,
                            void *data, int *got_frame_ptr,
                            AVPacket *avpkt)
{
    HAACDecContext *aac = avctx->priv_data;
    AVFrame *frame = data;
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
        info.sampRateCore = avctx->sample_rate;
        info.nChans  = avctx->channels;
        info.profile = AAC_PROFILE_LC;
        ret = AACSetRawBlockParams(aac->context, 0, &info);
        if (ret < 0)
            return ret;
    }

    ret = AACDecode(aac->context, &in_data, &in_size, (int16_t *)aac->pcm);
    if (ret < 0)
        return ret;

    AACGetLastFrameInfo(aac->context, &info);

    if (!avctx->sample_rate)
         avctx->sample_rate = info.sampRateOut;

    if (!avctx->channels)
         avctx->channels = info.nChans;

    if (!avctx->channel_layout)
         avctx->channel_layout = av_get_default_channel_layout(avctx->channels);

    avctx->frame_size = info.outputSamps / info.nChans;

    frame->nb_samples = avctx->frame_size;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    memcpy(frame->extended_data[0], aac->pcm,
           avctx->channels * avctx->frame_size *
           av_get_bytes_per_sample(avctx->sample_fmt));

    *got_frame_ptr = 1;

    return avpkt->size - in_size;
}

static av_cold int aac_decode_close(AVCodecContext *avctx)
{
    HAACDecContext *aac = avctx->priv_data;

    if (aac->context)
        AACFreeDecoder(aac->context);
    av_freep(&aac->pcm);

    return 0;
}

AVCodec ff_libhelix_aac_decoder = {
    .name                  = "libhelix_aac",
    .long_name             = NULL_IF_CONFIG_SMALL("libHelix AAC Decoder"),
    .type                  = AVMEDIA_TYPE_AUDIO,
    .id                    = AV_CODEC_ID_AAC,
    .priv_data_size        = sizeof(HAACDecContext),
    .init                  = aac_decode_init,
    .decode                = aac_decode_frame,
    .close                 = aac_decode_close,
    .capabilities          = AV_CODEC_CAP_DR1,
    .caps_internal         = FF_CODEC_CAP_INIT_THREADSAFE,
    .channel_layouts       = (const uint64_t[]) { AV_CH_LAYOUT_MONO,
                                                  AV_CH_LAYOUT_STEREO, 0},
    .sample_fmts           = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                             AV_SAMPLE_FMT_NONE },
};
