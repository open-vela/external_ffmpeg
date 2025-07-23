/*
 * Silk V3 Audio Decoder
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
 * Silk V3 Decoder Implementation
 */

#include "decode.h"
#include "avcodec.h"
#include "internal.h"
#include "codec_internal.h"
#include "libavutil/mem.h"
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"

#include <SKP_Silk_SDK_API.h>

#define MAX_INPUT_FRAMES 5
#define FRAME_LENGTH_MS  20
#define MAX_API_FS_KHZ   24

typedef struct SilkDecContext {
    AVClass *class;
    void *context;
} SilkDecContext;

static int silk_decode_init(AVCodecContext *avctx)
{
    SilkDecContext *silk = avctx->priv_data;
    int ret, size;

    ret = SKP_Silk_SDK_Get_Decoder_Size(&size);
    if (ret)
        return ret;

    silk->context = av_malloc(size);
    if (!silk->context)
        return AVERROR(ENOMEM);

    ret = SKP_Silk_SDK_InitDecoder(silk->context);
    if (ret)
        goto out;

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;

    return 0;

out:
    av_free(silk->context);
    return ret;
}

static int silk_decode_frame(AVCodecContext *avctx,
                            AVFrame *frame, int *got_frame_ptr,
                            AVPacket *avpkt)
{
    SilkDecContext *silk = avctx->priv_data;
    SKP_SILK_SDK_DecControlStruct control;
    short total = 0, once = 0, *out;
    int ret, frames = 0;

    frame->nb_samples = ((FRAME_LENGTH_MS * MAX_API_FS_KHZ) << 1) * MAX_INPUT_FRAMES /
                         (avctx->ch_layout.nb_channels * av_get_bytes_per_sample(avctx->sample_fmt));
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    control.API_sampleRate  = avctx->sample_rate;
    control.framesPerPacket = 1;
    out = (short *)frame->extended_data[0];

    do {
        ret = SKP_Silk_SDK_Decode(silk->context, &control, 0,
                                  avpkt->data, avpkt->size, out, &once);
        if (ret < 0)
            return ret;

        out   += once;
        total += once;
        frames++;

        if (frames >= MAX_INPUT_FRAMES)
            break;
    } while (control.moreInternalDecoderFrames);

    frame->nb_samples = total;
    *got_frame_ptr    = 1;

    return avpkt->size;
}

static av_cold int silk_decode_close(AVCodecContext *avctx)
{
    SilkDecContext *silk = avctx->priv_data;

    if (silk->context)
        av_free(silk->context);

    return 0;
}

static const AVChannelLayout silk_ch_layouts[] = {
    AV_CHANNEL_LAYOUT_MONO,
    { 0 }
};

const FFCodec ff_silk_decoder = {
    .p.name           = "silk",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Silk V3 Audio Decoder"),
    .p.type           = AVMEDIA_TYPE_AUDIO,
    .p.id             = AV_CODEC_ID_SILK,
    .p.capabilities   = AV_CODEC_CAP_DR1,
    .p.ch_layouts     = silk_ch_layouts,
    .p.sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                             AV_SAMPLE_FMT_NONE },
    .priv_data_size   = sizeof(SilkDecContext),
    .init             = silk_decode_init,
    FF_CODEC_DECODE_CB(silk_decode_frame),
    .close            = silk_decode_close,
};
