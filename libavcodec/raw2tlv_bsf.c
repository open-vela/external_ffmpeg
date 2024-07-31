/*
 * RAW data to TLV data bitstream filter
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
#include "libavcodec/codec_id.h"
#include "libavcodec/codec_par.h"
#include "libavcodec/avcodec.h"
#include "bsf.h"
#include "bsf_internal.h"

/****************************************************************************
 * TLV packet Definitions
 ****************************************************************************/
/*
 * +----+------------+-----------------------------+
 * |    |            |                             |
 * |type|   length   |               data          |
 * |    |            |                             |
 * +----+------------+-----------------------------+
 * |<- header size ->|
 *
 * Type:   1 byte           packet header type : RAW2TLV_TYPE_XXX
 * Length: 3 byte           data length. little-endian transmission
 * Data:   <Length> byte    the data of the corresponding type
 *
 */

/* TLV type  */
#define RAW2TLV_TYPE_DATA   0

/* TLV header size */
#define RAW2TLV_HEADER_SIZE 4

static int raw2tlv_wrap_payload(AVBSFContext *ctx, AVPacket *out_pkt)
{
    uint8_t *out_data, buf[RAW2TLV_HEADER_SIZE];
    PutBitContext pb;
    AVPacket *pkt;
    int ret;

    ret = ff_bsf_get_packet(ctx, &pkt);
    if (ret < 0)
        return ret;

    if (pkt->size == 0) {
        av_packet_move_ref(out_pkt, pkt);
        return 0;
    }

    ret = av_new_packet(out_pkt, RAW2TLV_HEADER_SIZE + pkt->size);
    if (ret < 0)
        goto fail;

    init_put_bits(&pb, buf, RAW2TLV_HEADER_SIZE);
    put_bits(&pb, 8, RAW2TLV_TYPE_DATA);
    put_bits(&pb, 24, pkt->size);
    flush_put_bits(&pb);

    out_data = out_pkt->data;
    memcpy(out_data, buf, RAW2TLV_HEADER_SIZE);

    out_data += RAW2TLV_HEADER_SIZE;
    memcpy(out_data, pkt->data, pkt->size);

    ret = av_packet_copy_props(out_pkt, pkt);
    if (ret < 0)
        goto fail;

    av_packet_free(&pkt);
    return 0;

fail:
    av_packet_unref(out_pkt);
    av_packet_free(&pkt);
    return ret;
}

static const enum AVCodecID raw2tlv_codec_ids[] = {
    AV_CODEC_ID_OPUS,
    AV_CODEC_ID_NONE,
};

const FFBitStreamFilter ff_raw2tlv_bsf = {
    .p.name      = "raw2tlv",
    .p.codec_ids = raw2tlv_codec_ids,
    .filter      = raw2tlv_wrap_payload,
};
