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
#include "codec_internal.h"
#include "decode.h"
#include "get_bits.h"
#include "internal.h"
#include "mpegaudiodecheader.h"
#include "libavutil/intreadwrite.h"

#include <smf_api.h>
#include <smf_debug.h>
#include <smf_codec_aac.h>
#include <smf_codec_mp3.h>
#include <smf_codec_sbc.h>

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

static int hifi_decode_pre_parse(AVCodecContext *avctx, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    int ret;

    switch (avctx->codec_id) {
        case AV_CODEC_ID_AAC: {
            AACADTSHeaderInfo adts;
            GetBitContext gb;

            if (!avctx->ch_layout.nb_channels) {
                if ((ret = init_get_bits8(&gb, buf, buf_size)) < 0)
                    return ret;

                if ((ret = ff_adts_header_parse(&gb, &adts)) < 0)
                    return ret;

                av_channel_layout_default(&avctx->ch_layout, ff_mpeg4audio_channels[adts.chan_config]);
            }
            break;
        }
        case AV_CODEC_ID_MP3: {
            uint32_t state = buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
            enum AVCodecID id;
            int r, c, s, b;

            ret = ff_mpa_decode_header(state, &r, &c, &s, &b, &id);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "%s h 0x%08x size %d ret %d\n", __func__, state, buf_size, ret);
                return AVERROR_INVALIDDATA;
            }

            av_channel_layout_default(&avctx->ch_layout, c);
            avctx->frame_size = s;
            break;
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

static int hifi_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame_ptr, AVPacket *avpkt)
{
    HIFIDecContext *hifi = avctx->priv_data;
    smf_media_info_t info = { 0 };
    smf_frame_t input, output;
    smf_error_t *error;
    int ret;

    if (!hifi)
        return AVERROR(EIO);

    ret = hifi_decode_pre_parse(avctx, avpkt);
    if (ret < 0)
        return ret;

    if (!avctx->ch_layout.nb_channels || avctx->sample_fmt == AV_SAMPLE_FMT_NONE)
        return AVERROR_INVALIDDATA;

    frame->nb_samples = hifi->out_size / (avctx->ch_layout.nb_channels * av_get_bytes_per_sample(avctx->sample_fmt));
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
    output.max  = frame->nb_samples * avctx->ch_layout.nb_channels *
                  av_get_bytes_per_sample(avctx->sample_fmt);

    if (!smf_decode(hifi->context, &input, &output)) {
        error = smf_get_error(hifi->context);
        av_log(avctx, AV_LOG_ERROR, "smf_decode failed: %d error %llu\n", __LINE__, error->err64);
        return AVERROR(error->err64);
    }

    /* error handling, drop this frame */
    if (!output.size) {
        av_log(avctx, AV_LOG_ERROR, "smf_decode failed: %d, in %d out %d\n", __LINE__, avpkt->size, output.size);
        return avpkt->size;
    }

    if (!smf_get(hifi->context, SMF_GET_MEDIA_INFO, (void**)&info))
        return AVERROR(ENOSYS);

    if (!avctx->sample_rate)
        avctx->sample_rate = info.sample_rate;

    if (!avctx->ch_layout.nb_channels)
        av_channel_layout_default(&avctx->ch_layout, info.channels);

    if (!avctx->frame_size)
        avctx->frame_size = output.size / (info.channels * av_get_bytes_per_sample(avctx->sample_fmt));

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

const FFCodec ff_hifi4_aac_decoder = {
    .p.name            = "hifi4_aac",
    .p.long_name       = NULL_IF_CONFIG_SMALL("HIFI4 AAC DECODER"),
    .p.type            = AVMEDIA_TYPE_AUDIO,
    .p.id              = AV_CODEC_ID_AAC,
    .priv_data_size    = sizeof(HIFIDecContext),
    .init              = hifi_decode_init,
    FF_CODEC_DECODE_CB(hifi_decode_frame),
    .close             = hifi_decode_close,
    .p.capabilities    = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .caps_internal     = FF_CODEC_CAP_INIT_THREADSAFE,
    .p.sample_fmts     = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                         AV_SAMPLE_FMT_NONE },
    .p.ch_layouts      = (const AVChannelLayout[]) { AV_CHANNEL_LAYOUT_MONO,
                                                     AV_CHANNEL_LAYOUT_STEREO, { 0 } },
};

const FFCodec ff_hifi4_mp3_decoder = {
    .p.name            = "hifi4_mp3",
    .p.long_name       = NULL_IF_CONFIG_SMALL("HIFI4 MP3 DECODER"),
    .p.type            = AVMEDIA_TYPE_AUDIO,
    .p.id              = AV_CODEC_ID_MP3,
    .priv_data_size    = sizeof(HIFIDecContext),
    .init              = hifi_decode_init,
    FF_CODEC_DECODE_CB(hifi_decode_frame),
    .close             = hifi_decode_close,
    .p.capabilities    = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .caps_internal     = FF_CODEC_CAP_INIT_THREADSAFE,
    .p.sample_fmts     = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                         AV_SAMPLE_FMT_NONE },
    .p.ch_layouts      = (const AVChannelLayout[]) { AV_CHANNEL_LAYOUT_MONO,
                                                     AV_CHANNEL_LAYOUT_STEREO, { 0 } },
};
