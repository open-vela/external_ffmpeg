/*
 * LC3 decoder
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
 * LC3(Low Complexity Communication Codec) decoder implementation
 */

#include <lc3.h>

#include "avcodec.h"
#include "decode.h"
#include "internal.h"
#include "codec_internal.h"

typedef struct LC3DecContext {
    AVClass *class;
    lc3_decoder_t *dec;
    int pcm_sbytes;
    int frame_bytes;
    enum lc3_pcm_format pcm_fmt;
} LC3DecContext;

static int lc3_decode_init(AVCodecContext *avctx)
{
    LC3DecContext *lc3ctx = avctx->priv_data;
    int nch = avctx->ch_layout.nb_channels;
    int srate_hz = avctx->sample_rate;
    lc3_decoder_t dec_mem;
    int frame_us, ich;

    frame_us = AV_TIME_BASE * avctx->frame_size / avctx->sample_rate;
    if (!LC3_CHECK_DT_US(frame_us)) {
        av_log(avctx, AV_LOG_ERROR, "Check frame duration failed!\n");
        return AVERROR_INVALIDDATA;
    }

    lc3ctx->frame_bytes = lc3_frame_bytes(frame_us, avctx->bit_rate / nch);
    lc3ctx->pcm_sbytes  = av_get_bytes_per_sample(avctx->sample_fmt);

    switch (avctx->sample_fmt) {
        case AV_SAMPLE_FMT_FLT:  lc3ctx->pcm_fmt = LC3_PCM_FORMAT_FLOAT; break;
        default:                 lc3ctx->pcm_fmt = LC3_PCM_FORMAT_S16;   break;
    }

    lc3ctx->dec = av_mallocz(nch * sizeof(lc3_decoder_t));
    if (!lc3ctx->dec)
        return AVERROR(ENOMEM);

    for (ich = 0; ich < nch; ich++) {
        dec_mem = av_malloc(lc3_decoder_size(frame_us, srate_hz));
        if (!dec_mem) {
            while(--ich >= 0)
                av_freep(&lc3ctx->dec[ich]);
            av_freep(&lc3ctx->dec);
            return AVERROR(ENOMEM);
        }

        lc3ctx->dec[ich] = lc3_setup_decoder(frame_us, srate_hz, srate_hz, dec_mem);
    }

    return 0;
}

static int lc3_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame_ptr, AVPacket *avpkt)
{
    LC3DecContext *lc3ctx = avctx->priv_data;
    int nch = avctx->ch_layout.nb_channels;
    int ret, ich;

    frame->nb_samples = avctx->frame_size;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
        return ret;
    }

    for (ich = 0; ich < nch; ich++) {
        lc3_decode(lc3ctx->dec[ich],
            avpkt->data + ich * lc3ctx->frame_bytes,
            lc3ctx->frame_bytes,
            lc3ctx->pcm_fmt,
            frame->extended_data[0] + ich * lc3ctx->pcm_sbytes,
            nch);
    }
    *got_frame_ptr = 1;

    return avpkt->size;
}

static int lc3_decode_close(AVCodecContext *avctx)
{
    LC3DecContext *lc3ctx = avctx->priv_data;
    int nch = avctx->ch_layout.nb_channels;
    int ich;

    for(ich = 0; ich < nch; ich++) {
        if (lc3ctx->dec[ich]) {
            av_freep(&lc3ctx->dec[ich]);
        }
    }
    av_freep(&lc3ctx->dec);

    return 0;
}

FFCodec ff_lc3_decoder = {
    .p.name            = "lc3",
    .p.long_name       = NULL_IF_CONFIG_SMALL("LC3 AUDIO Decoder"),
    .p.type            = AVMEDIA_TYPE_AUDIO,
    .p.id              = AV_CODEC_ID_LC3,
    .priv_data_size    = sizeof(LC3DecContext),
    .init              = lc3_decode_init,
    FF_CODEC_DECODE_CB(lc3_decode_frame),
    .close             = lc3_decode_close,
    .p.capabilities    = AV_CODEC_CAP_DR1,
    .caps_internal     = FF_CODEC_CAP_INIT_THREADSAFE,
    .p.sample_fmts     = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                         AV_SAMPLE_FMT_FLT,
                                                         AV_SAMPLE_FMT_NONE },
};
