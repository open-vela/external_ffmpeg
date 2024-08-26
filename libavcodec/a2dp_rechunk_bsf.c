/*
 * A2DP rechunk bitstream filter
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

#include "avcodec.h"
#include "bsf_internal.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include <libavcodec/avcodec.h>

typedef struct A2DPContext {
    AVCodecContext       *avctx;
    AVCodecParserContext *parser;
    AVPacket             *in_pkt;
} A2DPContext;

static int a2dp_rechunk_init(AVBSFContext *ctx)
{
    A2DPContext *s = ctx->priv_data;
    const AVCodec *codec;
    int ret;

    s->in_pkt = av_packet_alloc();
    if (!s->in_pkt)
        return AVERROR(ENOMEM);

    s->parser = av_parser_init(ctx->par_in->codec_id);
    if (!s->parser) {
        ret = AVERROR(EINVAL);
        goto error;
    }

    /* find the audio decoder */
    codec = avcodec_find_decoder(ctx->par_in->codec_id);
    if (!codec) {
        ret = AVERROR_DECODER_NOT_FOUND;
        goto error;
    }

    s->avctx = avcodec_alloc_context3(codec);
    if (!s->avctx) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    return 0;

error:
    av_packet_free(&s->in_pkt);
    av_parser_close(s->parser);
    return ret;
}

static void a2dp_rechunk_uninit(AVBSFContext *ctx)
{
    A2DPContext *s = ctx->priv_data;

    av_packet_free(&s->in_pkt);
    av_parser_close(s->parser);
    avcodec_free_context(&s->avctx);
}

static void a2dp_rechunk_flush(AVBSFContext *ctx)
{
    A2DPContext *s = ctx->priv_data;

    av_packet_unref(s->in_pkt);
}

static int a2dp_rechunk_filter(AVBSFContext *ctx, AVPacket *pkt)
{
    A2DPContext *s = ctx->priv_data;
    uint8_t *outbuf;
    int outbuf_size;
    int ret = 0;

    while(1) {
        if (!s->in_pkt->size) {
            ret = ff_bsf_get_packet_ref(ctx, s->in_pkt);
            if (ret < 0)
                break;
        }

        ret = av_parser_parse2(s->parser, s->avctx, &outbuf, &outbuf_size,
                            s->in_pkt->data, s->in_pkt->size,
                            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        /* if parser header error, return error; */
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "%s parser error :%d\n", __func__, ret);
            break;
        }

        s->in_pkt->data += ret;
        s->in_pkt->size -= ret;

        /* if not found end, parser had cached remaining data, unref current pkt */
        if (outbuf_size == 0 || outbuf == NULL) {
            av_assert0(s->in_pkt->size == 0);
            av_packet_unref(s->in_pkt);
            continue;
        }

        /* note: outbuf_size >= ret */
        /* get one packet from in_pkt or */
        /* cache combine with part in_pkt data */
        if (!s->in_pkt->size) {
            if (outbuf_size == ret)
                av_packet_move_ref(pkt, s->in_pkt);
            else
                av_packet_unref(s->in_pkt);
        }

        pkt->data = outbuf;
        pkt->size = outbuf_size;
        break;
    };

    return ret;
}

static const enum AVCodecID codec_ids[] = {
    AV_CODEC_ID_SBC,
    AV_CODEC_ID_SBC_PACKED,
    AV_CODEC_ID_SBC_PACKED_A2DP,
    AV_CODEC_ID_AAC,
    AV_CODEC_ID_AAC_LATM,
    AV_CODEC_ID_AAC_LATM_A2DP,
    AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_a2dp_rechunk_bsf = {
    .p.name           = "a2dp_rechunk",
    .priv_data_size = sizeof(A2DPContext),
    .filter         = a2dp_rechunk_filter,
    .init           = a2dp_rechunk_init,
    .flush          = a2dp_rechunk_flush,
    .close          = a2dp_rechunk_uninit,
    .p.codec_ids      = codec_ids,
};
