/*
 * MPEG-2/4 AAC RAW to MPEG-2/4 AAC adts bitstream filter
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

#define ADTS_HEADER_SIZE     7
#define ADTS_MAX_FRAME_BYTES ((1 << 14) - 1)

typedef struct ADTSContext {
    AVClass *class;
    int object_type;
    int sample_rate_index;
    int channel_conf;
} ADTSContext;

static const struct {
    int rate;
    int index;
} rate_index_table[] = {
    {96000, 0}, {88200, 1}, {64000, 2}, {48000, 3}, {44100, 4},
    {32000, 5}, {24000, 6}, {22050, 7}, {16000, 8}, {12000, 9},
    {11025, 10}, {8000, 11}, {7350, 12}
};

static int adts_get_samplerate_idx(int sample_rate)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(rate_index_table); i++) {
        if (rate_index_table[i].rate == sample_rate) {
            return rate_index_table[i].index;
        }
    }

    return 15; // frequency is written explictly
}

static int adts_get_chan_config(int channels)
{
    return channels >= 1 && channels <= 7 ? channels : 0;
}

static int adts_decode_extradata(AVBSFContext *ctx, ADTSContext *adts, AVCodecParameters *codecpar)
{
    adts->object_type       = 0;
    adts->sample_rate_index = adts_get_samplerate_idx(codecpar->sample_rate);
    adts->channel_conf      = adts_get_chan_config(codecpar->ch_layout.nb_channels);

    if (adts->sample_rate_index == 15) {
        av_log(ctx, AV_LOG_ERROR, "Escape sample rate index illegal in ADTS\n");
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static int adts_write_frame_header(ADTSContext *ctx,
                                   uint8_t *buf, int size)
{
    PutBitContext pb;

    uint32_t full_frame_size = ADTS_HEADER_SIZE + size;
    if (full_frame_size > ADTS_MAX_FRAME_BYTES) {
        av_log(ctx, AV_LOG_ERROR, "ADTS frame size too large: %u (max %d)\n",
               full_frame_size, ADTS_MAX_FRAME_BYTES);
        return AVERROR_INVALIDDATA;
    }

    init_put_bits(&pb, buf, ADTS_HEADER_SIZE);

    /* adts_fixed_header */
    put_bits(&pb, 12, 0xfff);   /* syncword */
    put_bits(&pb, 1, 0);        /* ID */
    put_bits(&pb, 2, 0);        /* layer */
    put_bits(&pb, 1, 1);        /* protection_absent */
    put_bits(&pb, 2, ctx->object_type); /* profile_object_type */
    put_bits(&pb, 4, ctx->sample_rate_index);
    put_bits(&pb, 1, 0);        /* private_bit */
    put_bits(&pb, 3, ctx->channel_conf); /* channel_configuration */
    put_bits(&pb, 1, 0);        /* original_copy */
    put_bits(&pb, 1, 0);        /* home */

    /* adts_variable_header */
    put_bits(&pb, 1, 0);        /* copyright_identification_bit */
    put_bits(&pb, 1, 0);        /* copyright_identification_start */
    put_bits(&pb, 13, full_frame_size); /* aac_frame_length */
    put_bits(&pb, 11, 0x7ff);   /* adts_buffer_fullness */
    put_bits(&pb, 2, 0);        /* number_of_raw_data_blocks_in_frame */

    flush_put_bits(&pb);
    return 0;
}

static int aac_rawtoadts_init(AVBSFContext *ctx)
{
    AVCodecParameters *par = ctx->par_in;
    ADTSContext *adts = ctx->priv_data;

    if (par->codec_id != AV_CODEC_ID_AAC) {
        av_log(ctx, AV_LOG_ERROR, "Only AAC, ADTS are supported\n");
        return AVERROR(EINVAL);
    }

    return adts_decode_extradata(ctx, adts, par);
}

static int aac_rawtoadts_filter(AVBSFContext *ctx, AVPacket *out_pkt)
{
    ADTSContext *adts = ctx->priv_data;
    uint8_t buf[ADTS_HEADER_SIZE];
    int outpkt_size, ret;
    uint8_t *out_data;
    AVPacket *pkt;

    ret = ff_bsf_get_packet(ctx, &pkt);
    if (ret < 0)
        return ret;

    if ((pkt->size > 2 && pkt->data[0] == 0xFF && (pkt->data[1] & 0xF0) == 0xF0) ||
         pkt->size == 0 || ctx->par_in->codec_id != AV_CODEC_ID_AAC) {
        av_packet_move_ref(out_pkt, pkt);
        return 0;
    }

    outpkt_size = pkt->size + ADTS_HEADER_SIZE;
    ret = av_new_packet(out_pkt, outpkt_size);
    if (ret < 0)
        goto fail;

    out_data = out_pkt->data;

    ret = adts_write_frame_header(adts, buf, pkt->size);
    if (ret < 0)
        goto fail;
    memcpy(out_data, buf, ADTS_HEADER_SIZE);
    out_data += ADTS_HEADER_SIZE;

    memcpy(out_data, pkt->data, pkt->size);
    ret = av_packet_copy_props(out_pkt, pkt);

fail:
    if (ret < 0)
        av_packet_unref(out_pkt);
    av_packet_free(&pkt);
    return ret;
}

static const AVClass aac_rawtoadts_class = {
    .class_name     = "aac_rawtoadts",
    .item_name      = av_default_item_name,
    .version        = LIBAVUTIL_VERSION_INT,
};

const FFBitStreamFilter ff_aac_rawtoadts_bsf = {
    .p.name           = "aac_rawtoadts",
    .p.priv_class     = &aac_rawtoadts_class,
    .priv_data_size   = sizeof(ADTSContext),
    .init             = aac_rawtoadts_init,
    .filter           = aac_rawtoadts_filter,
};