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
 * SBC encoder implementation
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "encode.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"

#include <sbc_encoder.h>

typedef struct SBCEncContext {
    AVClass        *class;
    SBC_ENC_PARAMS context;
    int64_t        max_delay;
    int            frame_length;
    const char     *sbc_param;
    int            nb_out_pkts;
} SBCEncContext;

static int sbc_encoder_parse_param(const char *sbc_param, SBC_ENC_PARAMS *param)
{
    AVDictionaryEntry *tag;
    AVDictionary    *format_opt = NULL;
    int ret;

    if (sbc_param == NULL || param == NULL)
        goto error;

    ret = av_dict_parse_string(&format_opt, sbc_param, "=", ":", 0);
    if (ret != 0)
        goto error;

    tag = av_dict_get(format_opt, "channel_mode", NULL, 0);
    if (tag == NULL)
        goto error;
    param->s16ChannelMode = strtoul(tag->value, NULL, 0);

    tag = av_dict_get(format_opt, "blocks", NULL, 0);
    if (tag == NULL)
        goto error;
    param->s16NumOfBlocks = strtoul(tag->value, NULL, 0);

    tag = av_dict_get(format_opt, "subbands", NULL, 0);
    if (tag == NULL)
        goto error;
    param->s16NumOfSubBands = strtoul(tag->value, NULL, 0);

    tag = av_dict_get(format_opt, "alloc_method", NULL, 0);
    if (tag == NULL)
        goto error;
    param->s16AllocationMethod = strtoul(tag->value, NULL, 0);

    tag = av_dict_get(format_opt, "bitpool", NULL, 0);
    if (tag == NULL)
        goto error;
    param->s16BitPool = strtoul(tag->value, NULL, 0);
    av_dict_free(&format_opt);

    return 0;

error:
    if (format_opt != NULL)
        av_dict_free(&format_opt);
    return AVERROR(EINVAL);
}

static int sbc_encode_init(AVCodecContext *avctx)
{
    SBCEncContext *sbc = avctx->priv_data;
    SBC_ENC_PARAMS *param = &sbc->context;
    uint8_t joint;
    uint8_t dual;

    if (avctx->profile == FF_PROFILE_SBC_MSBC) {
        if (avctx->ch_layout.nb_channels != 1) {
            av_log(avctx, AV_LOG_ERROR, "mSBC require mono channel.\n");
            return AVERROR(EINVAL);
        }

        if (avctx->sample_rate != 16000) {
            av_log(avctx, AV_LOG_ERROR, "mSBC require 16 kHz samplerate.\n");
            return AVERROR(EINVAL);
        }

        param->s16SamplingFreq = SBC_sf16000;
        param->s16ChannelMode = SBC_MONO;
        param->s16NumOfSubBands = 8;
        param->s16NumOfChannels = 1;
        param->s16NumOfBlocks = 15;
        param->s16AllocationMethod = SBC_LOUDNESS;
        param->s16BitPool = 26;
        param->Format = SBC_FORMAT_MSBC;

        avctx->frame_size = 8 * SBC_MAX_NUM_OF_BLOCKS;
    } else {
        if (avctx->global_quality > 255*FF_QP2LAMBDA) {
            av_log(avctx, AV_LOG_ERROR, "bitpool > 255 is not allowed.\n");
            return AVERROR(EINVAL);
        }

        if (sbc_encoder_parse_param(sbc->sbc_param, param) != 0) {
            if (avctx->ch_layout.nb_channels == 1) {
                param->s16ChannelMode = SBC_MONO;

                if (sbc->max_delay <= 3000 || avctx->bit_rate > 270000)
                    param->s16NumOfSubBands = 4;
                else
                    param->s16NumOfSubBands = 8;

            } else {
                if (avctx->bit_rate < 180000 || avctx->bit_rate > 420000)
                    param->s16ChannelMode = SBC_JOINT_STEREO;
                else
                    param->s16ChannelMode = SBC_STEREO;

                if (sbc->max_delay <= 4000 || avctx->bit_rate > 420000)
                    param->s16NumOfSubBands = 4;
                else
                    param->s16NumOfSubBands = 8;
            }

            /* sbc algorithmic delay is ((s16NumOfBlocks + 10) * s16NumOfSubBands - 2) / sample_rate */

            param->s16NumOfBlocks = av_clip(((sbc->max_delay * avctx->sample_rate + 2)
                                            / (1000000 * param->s16NumOfSubBands)) - 10, 4, 16) & ~3;

            param->s16AllocationMethod = SBC_LOUDNESS;
        }

        param->u16BitRate = avctx->bit_rate / 1000;
        param->s16NumOfChannels = avctx->ch_layout.nb_channels;

        switch (avctx->sample_rate)
        {
            case 16000:
                param->s16SamplingFreq = SBC_sf16000;
                break;
            case 32000:
                param->s16SamplingFreq = SBC_sf32000;
                break;
            case 44100:
                param->s16SamplingFreq = SBC_sf44100;
                break;
            default:
                param->s16SamplingFreq = SBC_sf48000;
                break;
        }

        avctx->frame_size = 4*((param->s16NumOfSubBands >> 3) + 1) * 4*(param->s16NumOfBlocks >> 2);

        SBC_Encoder_Init(param);
    }

    /* Calculate frame_length */

    joint = param->s16ChannelMode == SBC_JOINT_STEREO;
    dual  = param->s16ChannelMode == SBC_DUAL;
    sbc->frame_length = 4 + (4 * param->s16NumOfSubBands * param->s16NumOfChannels) / 8
                     + ((param->s16NumOfBlocks * param->s16BitPool * (1 + dual)
                     + joint * param->s16NumOfSubBands) + 7) / 8;

    avctx->frame_size *= sbc->nb_out_pkts;
    return 0;
}

static int sbc_encode_frame(AVCodecContext *avctx, AVPacket *avpkt,
                            const AVFrame *av_frame, int *got_packet_ptr)
{
    SBCEncContext *sbc = avctx->priv_data;
    char *pcm, *sbc_data;
    uint32_t encret;
    int ret, i;
    int per;

    /* input must be large enough to encode a complete frame */
    if (av_frame->nb_samples < avctx->frame_size)
        return 0;

    if ((ret = ff_alloc_packet(avctx, avpkt, sbc->frame_length * sbc->nb_out_pkts)) < 0)
        return ret;

    pcm = av_frame->extended_data[0];
    sbc_data = avpkt->data;
    per = av_frame->ch_layout.nb_channels * av_get_bytes_per_sample(av_frame->format);

    for (i = 0; i < sbc->nb_out_pkts; i++) {
        encret = SBC_Encode(&sbc->context, (int16_t *)pcm, sbc_data);
        if (encret != sbc->frame_length) {
            av_log(avctx, AV_LOG_ERROR, "SBC encode frame error, frame length %d, act %" PRIu32 "\n",
                   sbc->frame_length, encret);
            return AVERROR(EIO);
        }

        sbc_data += sbc->frame_length;
        pcm += avctx->frame_size * per / sbc->nb_out_pkts;
    }

    *got_packet_ptr = 1;
    return 0;
}

#define OFFSET(x) offsetof(SBCEncContext, x)
#define AE AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "sbc_delay", "set maximum algorithmic latency",
      OFFSET(max_delay), AV_OPT_TYPE_DURATION, {.i64 = 13000}, 1000,13000, AE },
    { "sbc_param", "", OFFSET(sbc_param), AV_OPT_TYPE_STRING, {.str=NULL}, AE },
    { "nb_out_pkts", "set out packets num", OFFSET(nb_out_pkts), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 32, AE },
    { NULL },
};

static const AVClass sbc_class = {
    .class_name = "sbc encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_libfluoride_sbc_encoder = {
    .p.name                  = "libfluoride_sbc",
    .p.long_name             = NULL_IF_CONFIG_SMALL("libfluoride SBC (low-complexity subband codec)"),
    .p.type                  = AVMEDIA_TYPE_AUDIO,
    .p.id                    = AV_CODEC_ID_SBC,
    .p.priv_class            = &sbc_class,
    .priv_data_size          = sizeof(SBCEncContext),
    .init                    = sbc_encode_init,
    FF_CODEC_ENCODE_CB(sbc_encode_frame),
    .p.capabilities          = AV_CODEC_CAP_SMALL_LAST_FRAME,
    .p.supported_samplerates = (const int[]) { 16000, 32000, 44100, 48000, 0 },
    .p.sample_fmts           = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16,
                                                               AV_SAMPLE_FMT_NONE },
    .p.ch_layouts            = (const AVChannelLayout[]) { AV_CHANNEL_LAYOUT_MONO,
                                                           AV_CHANNEL_LAYOUT_STEREO, { 0 } },
};
