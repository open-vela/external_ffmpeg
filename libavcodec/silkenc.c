/*
 * Silk V3 Audio Encoder
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
 * Silk V3 Audio Encoder Implementation
 */

#include "avcodec.h"
#include "internal.h"
#include "codec_internal.h"
#include "libavutil/mem.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"

#include <SKP_Silk_SDK_API.h>

#define MAX_BYTES_PER_FRAME 250
#define FRAME_LENGTH_MS     20
#define MAX_INPUT_FRAMES    5

typedef struct SilkEncContext {
    AVClass *class;
    void    *context;
} SilkEncContext;

static int silk_encode_init(AVCodecContext *avctx)
{
    SilkEncContext *silk = avctx->priv_data;
    SKP_SILK_SDK_EncControlStruct control;
    int size, ret;

    ret = SKP_Silk_SDK_Get_Encoder_Size(&size);
    if (ret)
        return ret;

    silk->context = av_malloc(size);
    if (!silk->context)
        return AVERROR(ENOMEM);

    ret = SKP_Silk_SDK_InitEncoder(silk->context, &control);
    if (ret)
        goto out;

    avctx->frame_size = FRAME_LENGTH_MS * avctx->sample_rate / 1000;

    return ret;

out:
    av_free(silk->context);
    return ret;
}

static int silk_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                            const AVFrame *av_frame, int *got_packet_ptr)
{
    SilkEncContext *silk = avctx->priv_data;
    SKP_SILK_SDK_EncControlStruct control;
    int ret;

    /* input must be large enough to encode a complete frame */
    if (av_frame->nb_samples < avctx->frame_size)
        return 0;

    ret = av_new_packet(avpkt, MAX_BYTES_PER_FRAME * MAX_INPUT_FRAMES);
    if (ret < 0)
        return ret;

    /* Set Encoder parameters */
    memset(&control, 0, sizeof(control));
    control.API_sampleRate        = avctx->sample_rate;
    control.maxInternalSampleRate = avctx->sample_rate;
    control.packetSize            = avctx->frame_size;
    control.bitRate               = avctx->bit_rate;

    ret = SKP_Silk_SDK_Encode(silk->context, &control,
                              (const SKP_int16 *)av_frame->data[0],
                              av_frame->nb_samples,
                              avpkt->data, (SKP_int16 *)&avpkt->size);
    if (ret)
        return ret;

    *got_packet_ptr = 1;
    return 0;
}

static int silk_encode_close(AVCodecContext *avctx)
{
    SilkEncContext *silk = avctx->priv_data;

    if (silk->context)
        av_free(silk->context);

    return 0;
}

static const AVChannelLayout silk_ch_layouts[] = {
    AV_CHANNEL_LAYOUT_MONO,
    { 0 }
};

const FFCodec ff_silk_encoder = {
    .p.name                  = "silk",
    .p.long_name             = NULL_IF_CONFIG_SMALL("Silk V3 Audio Encoder"),
    .p.type                  = AVMEDIA_TYPE_AUDIO,
    .p.id                    = AV_CODEC_ID_SILK,
    .p.capabilities          = AV_CODEC_CAP_SMALL_LAST_FRAME,
    .p.ch_layouts            = silk_ch_layouts,
    .p.sample_fmts           = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                             AV_SAMPLE_FMT_NONE },
    .p.supported_samplerates = (const int[]) { 16000, 0 },
    .priv_data_size          = sizeof(SilkEncContext),
    .init                    = silk_encode_init,
    FF_CODEC_ENCODE_CB(silk_encode_frame),
    .close                   = silk_encode_close,
};
