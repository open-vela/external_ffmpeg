/*
 * LC3 encoder
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
 * LC3(Low Complexity Communication Codec) encoder implementation
 */

#include <lc3.h>

#include "avcodec.h"
#include "encode.h"
#include "internal.h"
#include "codec_internal.h"

typedef struct LC3EncContext {
    AVClass *class;
    lc3_encoder_t *enc;
    int pcm_sbytes;
    int frame_bytes;
    enum lc3_pcm_format pcm_fmt;
} LC3EncContext;

static int lc3_encode_init(AVCodecContext *avctx)
{
    LC3EncContext *lc3ctx = avctx->priv_data;
    int nch = avctx->ch_layout.nb_channels;
    int srate_hz = avctx->sample_rate;
    int bitrate  = avctx->bit_rate;
    lc3_encoder_t enc_mem;
    int frame_us, ich;

    if (avctx->frame_size)
        frame_us = (double)avctx->frame_size / avctx->sample_rate * AV_TIME_BASE;
    if (!LC3_CHECK_DT_US(frame_us))
        frame_us = 10000; /* 10ms */

    avctx->frame_size   = lc3_frame_samples(frame_us, srate_hz);
    lc3ctx->frame_bytes = lc3_frame_bytes(frame_us, bitrate / nch);
    lc3ctx->pcm_sbytes  = av_get_bytes_per_sample(avctx->sample_fmt);

    switch (avctx->sample_fmt) {
        case AV_SAMPLE_FMT_FLT:
            lc3ctx->pcm_fmt = LC3_PCM_FORMAT_FLOAT;
            avctx->bits_per_raw_sample = 32;
            break;
        default:
            lc3ctx->pcm_fmt = LC3_PCM_FORMAT_S16;
            avctx->bits_per_raw_sample = 16;
            break;
    }

    lc3ctx->enc = av_mallocz(nch * sizeof(lc3_encoder_t));
    if (!lc3ctx->enc)
        return AVERROR(ENOMEM);

    for (ich = 0; ich < nch; ich++) {
        enc_mem = av_malloc(lc3_encoder_size(frame_us, srate_hz));
        if (!enc_mem) {
            while(--ich >= 0)
                av_freep(&lc3ctx->enc[ich]);
            av_freep(&lc3ctx->enc);
            return AVERROR(ENOMEM);
        }

        lc3ctx->enc[ich] = lc3_setup_encoder(frame_us, srate_hz, srate_hz, enc_mem);
    }

    return 0;
}

static int lc3_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                            const AVFrame *frame, int *got_frame_ptr)
{
    LC3EncContext *lc3ctx = avctx->priv_data;
    int nch = avctx->ch_layout.nb_channels;
    int ret, ich;

    ret = ff_alloc_packet(avctx, avpkt, nch * lc3ctx->frame_bytes);
    if (ret < 0)
        return ret;

    for (ich = 0; ich < nch; ich++) {
        lc3_encode(lc3ctx->enc[ich],
            lc3ctx->pcm_fmt,
            frame->data[0] + ich * lc3ctx->pcm_sbytes,
            nch,
            lc3ctx->frame_bytes,
            avpkt->data + ich * lc3ctx->frame_bytes);
    }
    *got_frame_ptr = 1;

    return 0;
}

static av_cold int lc3_encode_close(AVCodecContext *avctx)
{
    LC3EncContext *lc3ctx = avctx->priv_data;
    int nch = avctx->ch_layout.nb_channels;
    int ich;

    for(ich = 0; ich < nch; ich++) {
        if (lc3ctx->enc[ich]) {
          av_freep(&lc3ctx->enc[ich]);
        }
    }
    av_freep(&lc3ctx->enc);

    return 0;
}

FFCodec ff_lc3_encoder = {
    .p.name                  = "lc3",
    .p.long_name             = NULL_IF_CONFIG_SMALL("LC3 AUDIO Encoder"),
    .p.type                  = AVMEDIA_TYPE_AUDIO,
    .p.id                    = AV_CODEC_ID_LC3,
    .priv_data_size          = sizeof(LC3EncContext),
    .init                    = lc3_encode_init,
    FF_CODEC_ENCODE_CB(lc3_encode_frame),
    .close                   = lc3_encode_close,
    .caps_internal           = FF_CODEC_CAP_INIT_THREADSAFE,
    .p.capabilities          = AV_CODEC_CAP_DR1,
    .p.supported_samplerates = (const int[]) { 8000, 16000, 24000, 32000, 48000, 0 },
    .p.sample_fmts           = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                               AV_SAMPLE_FMT_FLT,
                                                               AV_SAMPLE_FMT_NONE },
};

