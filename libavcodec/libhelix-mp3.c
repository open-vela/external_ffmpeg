/*
 * libhelix mp3 decoder
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
 * libhelix MPEG decoder implementation
 */

#include "avcodec.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"

#include <mp3dec.h>

typedef struct HMP3DecContext {
    AVClass *class;
    HMP3Decoder context;
} HMP3DecContext;

static int mp3_decode_init(AVCodecContext *avctx)
{
    HMP3DecContext *mp3= avctx->priv_data;
    int ret;

    mp3->context = MP3InitDecoder();
    if (!mp3->context)
        return AVERROR(ENOMEM);

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    return 0;
}

static int mp3_decode_frame(AVCodecContext *avctx,
                            void *data, int *got_frame_ptr,
                            AVPacket *avpkt)
{
    HMP3DecContext *mp3 = avctx->priv_data;
    AVFrame *frame = data;
    MP3FrameInfo info;
    uint8_t *in_data;
    int ret, in_size;

    if (!mp3)
        return AVERROR(EIO);

    in_data = avpkt->data;
    in_size = avpkt->size;

    frame->nb_samples = MAX_NSAMP * MAX_NGRAN;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    ret = MP3Decode(mp3->context, &in_data, &in_size, (int16_t *)frame->extended_data[0], 0);
    if (ret < 0)
        return ret;

    MP3GetLastFrameInfo(mp3->context, &info);
    avctx->sample_rate = info.samprate;
    avctx->channels    = info.nChans;
    avctx->frame_size  = info.outputSamps / info.nChans;
    avctx->channel_layout = av_get_default_channel_layout(avctx->channels);

    *got_frame_ptr = 1;

    return avpkt->size - in_size;
}

static av_cold int mp3_decode_close(AVCodecContext *avctx)
{
    HMP3DecContext *mp3 = avctx->priv_data;

    if (mp3->context)
        MP3FreeDecoder(mp3->context);

    return 0;
}

AVCodec ff_libhelix_mp3_decoder = {
    .name                  = "libhelix_mp3",
    .long_name             = NULL_IF_CONFIG_SMALL("libHelix MPEG Decoder"),
    .type                  = AVMEDIA_TYPE_AUDIO,
    .id                    = AV_CODEC_ID_MP3,
    .priv_data_size        = sizeof(HMP3DecContext),
    .init                  = mp3_decode_init,
    .decode                = mp3_decode_frame,
    .close                 = mp3_decode_close,
    .capabilities          = AV_CODEC_CAP_DR1,
    .caps_internal         = FF_CODEC_CAP_INIT_THREADSAFE,
    .channel_layouts       = (const uint64_t[]) { AV_CH_LAYOUT_MONO,
                                                  AV_CH_LAYOUT_STEREO, 0},
    .sample_fmts           = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                             AV_SAMPLE_FMT_NONE },
};
