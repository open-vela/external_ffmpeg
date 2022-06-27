/*
 * MPEG-2/4 AAC RAW to MPEG-2/4 AAC LATM bitstream filter
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

#include "libavcodec/get_bits.h"
#include "libavcodec/put_bits.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/mpeg4audio.h"
#include "libavutil/opt.h"
#include "bsf.h"
#include "bsf_internal.h"

#define MAX_EXTRADATA_SIZE 1024

typedef struct LATMContext {
    int off;
    int channel_conf;
    int object_type;
} LATMContext;

static int latm_decode_extradata(AVBSFContext *s, uint8_t *buf, int size)
{
    LATMContext *ctx = s->priv_data;
    MPEG4AudioConfig m4ac;

    if (size > MAX_EXTRADATA_SIZE) {
        av_log(s, AV_LOG_ERROR, "Extradata is larger than currently supported.\n");
        return AVERROR_INVALIDDATA;
    }
    ctx->off = avpriv_mpeg4audio_get_config2(&m4ac, buf, size, 1, s);
    if (ctx->off < 0)
        return ctx->off;

    if (ctx->object_type == AOT_ALS && (ctx->off & 7)) {
        // as long as avpriv_mpeg4audio_get_config works correctly this is impossible
        av_log(s, AV_LOG_ERROR, "BUG: ALS offset is not byte-aligned\n");
        return AVERROR_INVALIDDATA;
    }
    /* FIXME: are any formats not allowed in LATM? */

    if (m4ac.object_type > AOT_SBR && m4ac.object_type != AOT_ALS) {
        av_log(s, AV_LOG_ERROR, "Muxing MPEG-4 AOT %d in LATM is not supported\n", m4ac.object_type);
        return AVERROR_INVALIDDATA;
    }
    ctx->channel_conf = m4ac.chan_config;
    ctx->object_type  = m4ac.object_type;

    return 0;
}

static void latm_write_frame_header(AVBSFContext *s, PutBitContext *bs)
{
    LATMContext *ctx = s->priv_data;
    AVCodecParameters *par = s->par_in;
    int header_size;

    /* AudioMuxElement */
    put_bits(bs, 1, 0);

    /* StreamMuxConfig */
    put_bits(bs, 1, 0); /* audioMuxVersion */
    put_bits(bs, 1, 1); /* allStreamsSameTimeFraming */
    put_bits(bs, 6, 0); /* numSubFrames */
    put_bits(bs, 4, 0); /* numProgram */
    put_bits(bs, 3, 0); /* numLayer */

    /* AudioSpecificConfig */
    if (ctx->object_type == AOT_ALS) {
        header_size = par->extradata_size - (ctx->off >> 3);
        avpriv_copy_bits(bs, &par->extradata[ctx->off >> 3], header_size);
    } else {
        // + 3 assumes not scalable and dependsOnCoreCoder == 0,
        // see decode_ga_specific_config in libavcodec/aacdec.c
        avpriv_copy_bits(bs, par->extradata, ctx->off + 3);

        if (!ctx->channel_conf) {
            GetBitContext gb;
            int ret = init_get_bits8(&gb, par->extradata, par->extradata_size);
            av_assert0(ret >= 0); // extradata size has been checked already, so this should not fail
            skip_bits_long(&gb, ctx->off + 3);
            ff_copy_pce_data(bs, &gb);
        }
    }

    put_bits(bs, 3, 0); /* frameLengthType */
    put_bits(bs, 8, 0xff); /* latmBufferFullness */

    put_bits(bs, 1, 0); /* otherDataPresent */
    put_bits(bs, 1, 0); /* crcCheckPresent */
}

static int aac_rawtolatm_init(AVBSFContext *s)
{
    AVCodecParameters *par = s->par_in;
    LATMContext *ctx = s->priv_data;

    if (par->codec_id == AV_CODEC_ID_AAC_LATM)
        return 0;
    if (par->codec_id != AV_CODEC_ID_AAC) {
        av_log(ctx, AV_LOG_ERROR, "Only AAC, LATM are supported\n");
        return AVERROR(EINVAL);
    }

    if (par->extradata_size > 0 &&
        latm_decode_extradata(s, par->extradata, par->extradata_size) < 0)
        return AVERROR_INVALIDDATA;

    return 0;
}

static int aac_rawtolatm_filter(AVBSFContext *bsfc, AVPacket *out_pkt)
{
    LATMContext *ctx = bsfc->priv_data;
    AVCodecParameters *par = bsfc->par_in;
    PutBitContext bs;
    AVPacket *pkt;
    int i, len;
    int outbuf_size;
    int ret;

    ret = ff_bsf_get_packet(bsfc, &pkt);
    if (ret < 0)
        return ret;

    if(pkt->size > 2 && pkt->data[0] == 0x56 && (pkt->data[1] >> 4) == 0xe &&
            (AV_RB16(pkt->data + 1) & 0x1FFF) + 3 == pkt->size) {
        av_packet_move_ref(out_pkt, pkt);
        return 0;
    }

    outbuf_size = pkt->size + 1024 + MAX_EXTRADATA_SIZE;
    ret = av_new_packet(out_pkt, outbuf_size);
    if (ret < 0)
        goto fail;

    init_put_bits(&bs, out_pkt->data + 3, outbuf_size - 3);
    latm_write_frame_header(bsfc, &bs);

    /* PayloadLengthInfo() */
    for (i = 0; i <= pkt->size - 255; i += 255)
        put_bits(&bs, 8, 255);

    put_bits(&bs, 8, pkt->size - i);

    /* The LATM payload is written unaligned */

    /* PayloadMux() */
    if (pkt->size && (pkt->data[0] & 0xe1) == 0x81) {
        // Convert byte-aligned DSE to non-aligned.
        // Due to the input format encoding we know that
        // it is naturally byte-aligned in the input stream,
        // so there are no padding bits to account for.
        // To avoid having to add padding bits and rearrange
        // the whole stream we just remove the byte-align flag.
        // This allows us to remux our FATE AAC samples into latm
        // files that are still playable with minimal effort.
        put_bits(&bs, 8, pkt->data[0] & 0xfe);
        avpriv_copy_bits(&bs, pkt->data + 1, 8*pkt->size - 8);
    } else
        avpriv_copy_bits(&bs, pkt->data, 8*pkt->size);

    avpriv_align_put_bits(&bs);
    flush_put_bits(&bs);

    len = put_bits_count(&bs) >> 3;
    out_pkt->data[0] = 0x56;
    out_pkt->data[1] = 0xe0 | ((len >> 8) & 0x1f);
    out_pkt->data[2] = len & 0xff;
    out_pkt->size = len + 3;
    ret = av_packet_copy_props(out_pkt, pkt);

fail:
    if (ret < 0)
        av_packet_unref(out_pkt);
    av_packet_free(&pkt);
    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_AAC, AV_CODEC_ID_AAC_LATM, AV_CODEC_ID_NONE,
};

const AVBitStreamFilter ff_aac_rawtolatm_bsf = {
    .name           = "aac_rawtolatm",
    .priv_data_size = sizeof(LATMContext),
    .init           = aac_rawtolatm_init,
    .filter         = aac_rawtolatm_filter,
    .codec_ids      = codec_ids,
};