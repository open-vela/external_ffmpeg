/*
 * Bluetooth low-complexity, subband codec (SBC)
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
 * SBC decoder implementation
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"

#include <oi_codec_sbc.h>

#define MSBC_MAX_BLOCKS 15
#define SBC_WBS_SAMPLES_PER_FRAME (SBC_MAX_BANDS * SBC_MAX_BLOCKS)
#define MSBC_WBS_SAMPLES_PER_FRAME (SBC_MAX_BANDS * MSBC_MAX_BLOCKS)

#define DECODER_DATA_SIZE (SBC_MAX_CHANNELS * SBC_MAX_BLOCKS * SBC_MAX_BANDS * 4 \
        + SBC_CODEC_MIN_FILTER_BUFFERS * SBC_MAX_BANDS * SBC_MAX_CHANNELS * 2)

typedef struct SBCDecContext {
    AVClass                      *class;
    uint8_t                      data[DECODER_DATA_SIZE + 3];
    OI_CODEC_SBC_DECODER_CONTEXT context;
} SBCDecContext;

static int sbc_decode_init(AVCodecContext *avctx)
{
    SBCDecContext *sbc = avctx->priv_data;
    OI_STATUS status;

    /*
     *msbc sample size is fixed to 120 by the sbc parser,
     *and we use this to determine whether it is msbc.
     *sbc sample size is usually 128.
     */
    if (avctx->frame_size == MSBC_WBS_SAMPLES_PER_FRAME) {
        status = OI_CODEC_SBC_DecoderReset(&sbc->context, (uint32_t *)sbc->data,
                                           sizeof(sbc->data), 1, avctx->ch_layout.nb_channels, false);
        if (!OI_SUCCESS(status))
            return AVERROR(status);

        status = OI_CODEC_SBC_DecoderConfigureMSbc(&sbc->context);
        if (!OI_SUCCESS(status))
            return AVERROR(status);
    } else {
        status = OI_CODEC_SBC_DecoderReset(&sbc->context, (uint32_t *)sbc->data,
                                           sizeof(sbc->data), 2, avctx->ch_layout.nb_channels, false);
        if (!OI_SUCCESS(status))
            return AVERROR(status);
    }

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    return 0;
}

static int sbc_packed_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                                   int *got_frame_ptr, AVPacket *avpkt)
{
    SBCDecContext *sbc = avctx->priv_data;
    const OI_BYTE* in_data;
    uint32_t in_size, out_avail;
    uint8_t *out_ptr;
    int nframes;
    int ret;
    int i;

    if (!sbc)
        return AVERROR(EIO);

    nframes = avpkt->data[0] & 0xf;
    if (avpkt->data[0] == OI_SBC_MSBC_SYNCWORD)
        frame->nb_samples = nframes * MSBC_WBS_SAMPLES_PER_FRAME;
    else if (avpkt->data[0] == OI_SBC_SYNCWORD)
        frame->nb_samples = nframes * SBC_WBS_SAMPLES_PER_FRAME;
    else
        return AVERROR(EINVAL);
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    in_data = avpkt->data + 1;
    in_size = avpkt->size - 1;
    out_ptr = frame->extended_data[0];
    out_avail = frame->linesize[0];

    for (i = 0; i < nframes; i++) {
        uint32_t out_size = out_avail;
        OI_STATUS status = OI_CODEC_SBC_DecodeFrame(&sbc->context, &in_data,
                                                    &in_size, (int16_t *)out_ptr, &out_size);
        if (!OI_SUCCESS(status)) {
            av_log(avctx, AV_LOG_ERROR, "SBC decode failed, status:%d\n", status);
            return AVERROR(status);
        }

        out_avail -= out_size;
        out_ptr   += out_size;
    }

    if (in_size)
        return AVERROR(EINVAL);

    *got_frame_ptr = 1;

    return avpkt->size;
}

static int sbc_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame_ptr, AVPacket *avpkt)
{
    SBCDecContext *sbc = avctx->priv_data;
    const OI_BYTE* in_data;
    uint32_t in_size, out_size;
    OI_STATUS status;
    int ret;

    if (!sbc)
        return AVERROR(EIO);

    if (avpkt->data[0] == OI_SBC_MSBC_SYNCWORD)
        frame->nb_samples = MSBC_WBS_SAMPLES_PER_FRAME;
    else if (avpkt->data[0] == OI_SBC_SYNCWORD)
        frame->nb_samples = SBC_WBS_SAMPLES_PER_FRAME;
    else
        return AVERROR(EINVAL);

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    in_data = avpkt->data;
    in_size = avpkt->size;
    out_size = frame->linesize[0];
    status = OI_CODEC_SBC_DecodeFrame(&sbc->context, &in_data,
                                      &in_size, (int16_t *)frame->extended_data[0], &out_size);
    if (!OI_SUCCESS(status)) {
        av_log(avctx, AV_LOG_ERROR, "%s, status: %d, in_size:%d\n", __func__, status, avpkt->size);
        return AVERROR(status);
    }

    *got_frame_ptr = 1;

    return avpkt->size - in_size;
}

const FFCodec ff_libfluoride_sbc_decoder = {
    .p.name                  = "libfluoride_sbc",
    .p.long_name             = NULL_IF_CONFIG_SMALL("libfluoride SBC (low-complexity subband codec)"),
    .p.type                  = AVMEDIA_TYPE_AUDIO,
    .p.id                    = AV_CODEC_ID_SBC,
    .priv_data_size          = sizeof(SBCDecContext),
    .init                    = sbc_decode_init,
    FF_CODEC_DECODE_CB(sbc_decode_frame),
    .p.capabilities          = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .p.supported_samplerates = (const int[]) { 16000, 32000, 44100, 48000, 0 },
    .p.sample_fmts           = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                               AV_SAMPLE_FMT_NONE },
    .p.ch_layouts            = (const AVChannelLayout[]) { AV_CHANNEL_LAYOUT_MONO,
                                                           AV_CHANNEL_LAYOUT_STEREO, { 0 } },
};

const FFCodec ff_libfluoride_sbc_packed_decoder = {
    .p.name                  = "libfluoride_sbc-packed",
    .p.long_name             = NULL_IF_CONFIG_SMALL("libfluoride SBC packed (low-complexity subband codec)"),
    .p.type                  = AVMEDIA_TYPE_AUDIO,
    .p.id                    = AV_CODEC_ID_SBC_PACKED,
    .priv_data_size          = sizeof(SBCDecContext),
    .init                    = sbc_decode_init,
    FF_CODEC_DECODE_CB(sbc_packed_decode_frame),
    .p.capabilities          = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .p.supported_samplerates = (const int[]) { 16000, 32000, 44100, 48000, 0 },
    .p.sample_fmts           = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                               AV_SAMPLE_FMT_NONE },
    .p.ch_layouts            = (const AVChannelLayout[]) { AV_CHANNEL_LAYOUT_MONO,
                                                           AV_CHANNEL_LAYOUT_STEREO, { 0 } },
};
