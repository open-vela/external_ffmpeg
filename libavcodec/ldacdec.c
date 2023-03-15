/*
 * LDAC decoder
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
 * LDAC decoder implementation
 */

#include "avcodec.h"
#include "decode.h"
#include "codec_internal.h"
#include "ldacBT.h"

#define LDAC_MIN_BUF_SIZE  128
#define LDAC_MID_BUF_SIZE  256
#define LDAC_MAX_BUF_SIZE  512

#define CASE_RETURN_STR(err_code) \
    case err_code:                \
        return #err_code;

typedef struct LdacDecContext {
    AVClass *class;
    HANDLE_LDAC_BT handle_ldac_bt;
    LDACBT_SMPL_FMT_T sample_fmt;
    int nb_samples;
} LdacDecContext;

static const char *ldac_errcode2str(int ErrCode)
{
    switch (ErrCode) {
        CASE_RETURN_STR(LDACBT_ERR_NONE);
        CASE_RETURN_STR(LDACBT_ERR_NON_FATAL);
        CASE_RETURN_STR(LDACBT_ERR_BIT_ALLOCATION);
        CASE_RETURN_STR(LDACBT_ERR_NOT_IMPLEMENTED);
        CASE_RETURN_STR(LDACBT_ERR_NON_FATAL_ENCODE);
        CASE_RETURN_STR(LDACBT_ERR_FATAL);
        CASE_RETURN_STR(LDACBT_ERR_SYNTAX_BAND);
        CASE_RETURN_STR(LDACBT_ERR_SYNTAX_GRAD_A);
        CASE_RETURN_STR(LDACBT_ERR_SYNTAX_GRAD_B);
        CASE_RETURN_STR(LDACBT_ERR_SYNTAX_GRAD_C);
        CASE_RETURN_STR(LDACBT_ERR_SYNTAX_GRAD_D);
        CASE_RETURN_STR(LDACBT_ERR_SYNTAX_GRAD_E);
        CASE_RETURN_STR(LDACBT_ERR_SYNTAX_IDSF);
        CASE_RETURN_STR(LDACBT_ERR_SYNTAX_SPEC);
        CASE_RETURN_STR(LDACBT_ERR_BIT_PACKING);
        CASE_RETURN_STR(LDACBT_ERR_ALLOC_MEMORY);
        CASE_RETURN_STR(LDACBT_ERR_FATAL_HANDLE);
        CASE_RETURN_STR(LDACBT_ERR_ILL_SYNCWORD);
        CASE_RETURN_STR(LDACBT_ERR_ILL_SMPL_FORMAT);
        CASE_RETURN_STR(LDACBT_ERR_ILL_PARAM);
        CASE_RETURN_STR(LDACBT_ERR_ASSERT_SAMPLING_FREQ);
        CASE_RETURN_STR(LDACBT_ERR_ASSERT_SUP_SAMPLING_FREQ);
        CASE_RETURN_STR(LDACBT_ERR_CHECK_SAMPLING_FREQ);
        CASE_RETURN_STR(LDACBT_ERR_ASSERT_CHANNEL_CONFIG);
        CASE_RETURN_STR(LDACBT_ERR_CHECK_CHANNEL_CONFIG);
        CASE_RETURN_STR(LDACBT_ERR_ASSERT_FRAME_LENGTH);
        CASE_RETURN_STR(LDACBT_ERR_ASSERT_SUP_FRAME_LENGTH);
        CASE_RETURN_STR(LDACBT_ERR_ASSERT_FRAME_STATUS);
        CASE_RETURN_STR(LDACBT_ERR_ASSERT_NSHIFT);
        CASE_RETURN_STR(LDACBT_ERR_ASSERT_CHANNEL_MODE);
        CASE_RETURN_STR(LDACBT_ERR_ENC_INIT_ALLOC);
        CASE_RETURN_STR(LDACBT_ERR_ENC_ILL_GRADMODE);
        CASE_RETURN_STR(LDACBT_ERR_ENC_ILL_GRADPAR_A);
        CASE_RETURN_STR(LDACBT_ERR_ENC_ILL_GRADPAR_B);
        CASE_RETURN_STR(LDACBT_ERR_ENC_ILL_GRADPAR_C);
        CASE_RETURN_STR(LDACBT_ERR_ENC_ILL_GRADPAR_D);
        CASE_RETURN_STR(LDACBT_ERR_ENC_ILL_NBANDS);
        CASE_RETURN_STR(LDACBT_ERR_PACK_BLOCK_FAILED);
        CASE_RETURN_STR(LDACBT_ERR_DEC_INIT_ALLOC);
        CASE_RETURN_STR(LDACBT_ERR_INPUT_BUFFER_SIZE);
        CASE_RETURN_STR(LDACBT_ERR_UNPACK_BLOCK_FAILED);
        CASE_RETURN_STR(LDACBT_ERR_UNPACK_BLOCK_ALIGN);
        CASE_RETURN_STR(LDACBT_ERR_UNPACK_FRAME_ALIGN);
        CASE_RETURN_STR(LDACBT_ERR_FRAME_LENGTH_OVER);
        CASE_RETURN_STR(LDACBT_ERR_FRAME_ALIGN_OVER);
        CASE_RETURN_STR(LDACBT_ERR_ALTER_EQMID_LIMITED);
        CASE_RETURN_STR(LDACBT_ERR_ILL_EQMID);
        CASE_RETURN_STR(LDACBT_ERR_ILL_SAMPLING_FREQ);
        CASE_RETURN_STR(LDACBT_ERR_ILL_NUM_CHANNEL);
        CASE_RETURN_STR(LDACBT_ERR_ILL_MTU_SIZE);
        CASE_RETURN_STR(LDACBT_ERR_HANDLE_NOT_INIT);
    default:
        return "unknown-error-code";
    }
}

static av_cold int ldac_decode_init(AVCodecContext *avctx)
{
    LdacDecContext *s = avctx->priv_data;
    int channel_size;
    int error_code;
    int cm;

    cm = LDACBT_CHANNEL_MODE_STEREO;
    if (avctx->ch_layout.nb_channels == 1)
        cm = LDACBT_CHANNEL_MODE_MONO;

    switch (avctx->sample_fmt) {
    case AV_SAMPLE_FMT_S16:
        s->sample_fmt = LDACBT_SMPL_FMT_S16;
        break;
    case AV_SAMPLE_FMT_S32:
        s->sample_fmt = LDACBT_SMPL_FMT_S32;
        break;
    case AV_SAMPLE_FMT_FLT:
        s->sample_fmt = LDACBT_SMPL_FMT_F32;
        break;
    default:
        av_log(s, AV_LOG_ERROR, "Unsupported fmt %d!\n", avctx->sample_fmt);
        return AVERROR(EINVAL);
    }

    switch (avctx->sample_rate) {
    case 44100:
    case 48000:
        s->nb_samples = LDAC_MIN_BUF_SIZE;
        break;
    case 88200:
    case 96000:
        s->nb_samples = LDAC_MID_BUF_SIZE;
        break;
    case 176400:
    case 192000:
        s->nb_samples = LDAC_MAX_BUF_SIZE;
        break;
    default:
        av_log(s, AV_LOG_ERROR, "Unsupported sample rate %d!\n", avctx->sample_rate);
        return AVERROR(EINVAL);
    }

    s->handle_ldac_bt = ldacBT_get_handle();
    if (!s->handle_ldac_bt) {
        av_log(s, AV_LOG_ERROR, "Can not Get LDAC Handle!\n");
        return AVERROR(ENOMEM);
    }

    if (ldacBT_init_handle_decode(s->handle_ldac_bt, cm, avctx->sample_rate, 0, 0, 0) < 0) {
        error_code = ldacBT_get_error_code(s->handle_ldac_bt);

        av_log(s, AV_LOG_ERROR, "Initializing LDAC Handle for synthesis!"
               "Error code API:%s, Handle:%s, Block:%s\n",
               ldac_errcode2str(LDACBT_API_ERR(error_code)),
               ldac_errcode2str(LDACBT_HANDLE_ERR(error_code)),
               ldac_errcode2str(LDACBT_BLOCK_ERR(error_code)));
        return AVERROR(EINVAL);
    }

    return 0;
}

static int ldac_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                             int *got_frame_ptr, AVPacket *avpkt)
{
    LdacDecContext *s = avctx->priv_data;
    int got_bytes;
    int used_bytes;
    int ret;

    frame->nb_samples = s->nb_samples;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    ret = ldacBT_decode(s->handle_ldac_bt, avpkt->data, frame->data[0],
                        s->sample_fmt, avpkt->size, &used_bytes, &got_bytes);
    if (ret) {
        av_log(s, AV_LOG_ERROR, "LDAC audio frame error!\n");
        return ret;
    }

    *got_frame_ptr = 1;

    return avpkt->size;
}

static int ldac_decode_close(AVCodecContext *avctx)
{
    LdacDecContext *s = avctx->priv_data;
    ldacBT_close_handle(s->handle_ldac_bt);
    ldacBT_free_handle(s->handle_ldac_bt);
    s->handle_ldac_bt = NULL;
    return 0;
}

const FFCodec ff_ldac_decoder = {
    .p.name         = "ldac",
    .p.long_name    = NULL_IF_CONFIG_SMALL("LDAC AUDIO Decoder"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_LDAC,
    .priv_data_size = sizeof(LdacDecContext),
    .init           = ldac_decode_init,
    FF_CODEC_DECODE_CB(ldac_decode_frame),
    .close          = ldac_decode_close,
    .p.capabilities = AV_CODEC_CAP_CHANNEL_CONF | AV_CODEC_CAP_DR1,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
    .p.sample_fmts  = (const enum AVSampleFormat[]){ AV_SAMPLE_FMT_S16,
                                                     AV_SAMPLE_FMT_S32,
                                                     AV_SAMPLE_FMT_FLT,
                                                     AV_SAMPLE_FMT_NONE },
};
