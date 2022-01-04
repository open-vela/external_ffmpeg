/*
 * audio codec optimized by HIFI4
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
 * HIFI4 implementation
 */

#include "adts_header.h"
#include "mpeg4audio.h"
#include "avcodec.h"
#include "get_bits.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"

#include <smf_api.h>
#include <smf_debug.h>
#include <smf_codec_aac.h>
#include <smf_codec_mp3.h>
#include <smf_codec_sbc.h>

#define AAC_SAMPLES_PER_FRAME 2048
#define MP3_SAMPLES_PER_FRAME 1152
#define SBC_SAMPLES_PER_FRAME 128

typedef struct HIFIDecContext {
    AVClass *class;
    void    *context;
    int     out_size;
} HIFIDecContext;

static int hifi_smf_open(AVCodecContext *avctx)
{
    HIFIDecContext *hifi = avctx->priv_data;
    int params[32] = { 0 };
    smf_error_t *error;
    int *ptr = params;

    switch (avctx->codec_id) {
        case AV_CODEC_ID_AAC: {
            smf_aac_dec_open_param_t *aac_param = (smf_aac_dec_open_param_t *)params;
            aac_param->media.package = SMF_AAC_PACKAGE_ADTS;
            aac_param->aot = SMF_AAC_AOT_LC;

            break;
        }
        case AV_CODEC_ID_MP3: {
            break;
        }
        case AV_CODEC_ID_SBC: {
            break;
        }
        default:
            return AVERROR(ENOSYS);
    }

    if (!smf_open(hifi->context, ptr)) {
        error = smf_get_error(hifi->context);
        av_log(avctx, AV_LOG_ERROR, "smf_open failed: %d err %llu\n", __LINE__, error->err64);
        return AVERROR(error->err64);
    }

    hifi->out_size = smf_get_output_maxsize(hifi->context);
    return 0;
}

static int hifi_decode_get_samples(AVCodecContext *avctx, AVPacket *avpkt)
{
    HIFIDecContext *hifi = avctx->priv_data;
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    AACADTSHeaderInfo adts;
    GetBitContext gb;
    int ret;

    switch (avctx->codec_id) {
        case AV_CODEC_ID_AAC: {
            if (!avctx->channels) {
                if ((ret = init_get_bits8(&gb, buf, buf_size)) < 0)
                    return ret;

                if ((ret = ff_adts_header_parse(&gb, &adts)) < 0)
                    return ret;

                avctx->channels = ff_mpeg4audio_channels[adts.chan_config];
            }

            return AAC_SAMPLES_PER_FRAME;
        }
        case AV_CODEC_ID_MP3: {
            return MP3_SAMPLES_PER_FRAME;
        }

        case AV_CODEC_ID_SBC: {
            return SBC_SAMPLES_PER_FRAME;
        }

        default: {
            return AVERROR(ENOSYS);
        }
    }
    return 0;
}

static int hifi_decode_init(AVCodecContext *avctx)
{
    HIFIDecContext *hifi = avctx->priv_data;
    int ret;

    switch (avctx->codec_id) {
        case AV_CODEC_ID_AAC:
            smf_aac_decoder_register();
            hifi->context = smf_create_decoder("aac");
            break;

        case AV_CODEC_ID_MP3:
            smf_mp3_decoder_register();
            hifi->context = smf_create_decoder("mp3");
            break;

        case AV_CODEC_ID_SBC:
            break;
    }

    if (!hifi->context)
        return AVERROR_UNKNOWN;

    if ((ret = hifi_smf_open(avctx)) < 0)
        return ret;

    avctx->sample_fmt = AV_SAMPLE_FMT_S16;
    return 0;
}

static int hifi_decode_frame(AVCodecContext *avctx,
                            void *data, int *got_frame_ptr,
                            AVPacket *avpkt)
{
    HIFIDecContext *hifi = avctx->priv_data;
    smf_media_info_t info = { 0 };
    smf_frame_t input, output;
    AVFrame *frame = data;
    smf_error_t *error;
    int ret;

    if (!hifi)
        return AVERROR(EIO);

    frame->nb_samples = hifi_decode_get_samples(avctx, avpkt);
    if (frame->nb_samples < 0)
        return frame->nb_samples;

    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    if (frame->linesize[0] < hifi->out_size) {
        av_log(avctx, AV_LOG_ERROR, "smf prepare out buffer failed %d %d\n", frame->linesize[0],  hifi->out_size);
        return AVERROR_INVALIDDATA;
    }

    memset(&input,  0, sizeof(smf_frame_t));
    memset(&output, 0, sizeof(smf_frame_t));

    input.buff = avpkt->data;
    input.size = avpkt->size;
    input.max  = avpkt->size;

    output.buff = frame->extended_data[0];
    output.max  = frame->nb_samples * avctx->channels *
                  av_get_bytes_per_sample(avctx->sample_fmt);

    if (!smf_decode(hifi->context, &input, &output)) {
        error = smf_get_error(hifi->context);
        av_log(avctx, AV_LOG_ERROR, "smf_decode failed: %d error %llu\n", __LINE__, error->err64);
        return AVERROR(error->err64);
    }

    if (!smf_get(hifi->context, SMF_GET_MEDIA_INFO, (void**)&info))
        return AVERROR(ENOSYS);

    avctx->sample_rate = info.sample_rate;
    avctx->frame_size  = output.size / (info.channels * av_get_bytes_per_sample(avctx->sample_fmt));
    avctx->channels = info.channels;
    avctx->channel_layout = av_get_default_channel_layout(avctx->channels);

    frame->nb_samples = avctx->frame_size;
    *got_frame_ptr = 1;

    return avpkt->size;
}

static av_cold int hifi_decode_close(AVCodecContext *avctx)
{
    HIFIDecContext *hifi = avctx->priv_data;

    if (hifi->context) {
        smf_close(hifi->context);
        smf_destroy(hifi->context);
    }

    return 0;
}

AVCodec ff_hifi4_aac_decoder = {
    .name            = "hifi4_aac",
    .long_name       = NULL_IF_CONFIG_SMALL("HIFI4 AAC DECODER"),
    .type            = AVMEDIA_TYPE_AUDIO,
    .id              = AV_CODEC_ID_AAC,
    .priv_data_size  = sizeof(HIFIDecContext),
    .init            = hifi_decode_init,
    .decode          = hifi_decode_frame,
    .close           = hifi_decode_close,
    .capabilities    = AV_CODEC_CAP_DR1,
    .caps_internal   = FF_CODEC_CAP_INIT_THREADSAFE,
    .sample_fmts     = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                       AV_SAMPLE_FMT_NONE },
    .channel_layouts = (const uint64_t[]) { AV_CH_LAYOUT_MONO,
                                            AV_CH_LAYOUT_STEREO, 0},
};

AVCodec ff_hifi4_mp3_decoder = {
    .name            = "hifi4_mp3",
    .long_name       = NULL_IF_CONFIG_SMALL("HIFI4 MP3 DECODER"),
    .type            = AVMEDIA_TYPE_AUDIO,
    .id              = AV_CODEC_ID_MP3,
    .priv_data_size  = sizeof(HIFIDecContext),
    .init            = hifi_decode_init,
    .decode          = hifi_decode_frame,
    .close           = hifi_decode_close,
    .capabilities    = AV_CODEC_CAP_DR1,
    .caps_internal   = FF_CODEC_CAP_INIT_THREADSAFE,
    .sample_fmts     = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                       AV_SAMPLE_FMT_NONE },
    .channel_layouts = (const uint64_t[]) { AV_CH_LAYOUT_MONO,
                                            AV_CH_LAYOUT_STEREO, 0},
};
