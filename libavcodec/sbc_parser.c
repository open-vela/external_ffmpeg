/*
 * SBC parser
 *
 * Copyright (C) 2017  Aurelien Jacobs <aurel@gnuage.org>
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

#include "sbc.h"
#include "parser.h"

typedef struct SBCParseContext {
    ParseContext pc;
    uint8_t      header[3];
    int          frame_len;
    int          buffered_size;
} SBCParseContext;

static int max_bitpool(int mode, int subbands)
{
    switch (mode) {
        case SBC_MODE_MONO:
        case SBC_MODE_DUAL_CHANNEL:
            return 16 * subbands;
        case SBC_MODE_STEREO:
        case SBC_MODE_JOINT_STEREO:
            return 32 * subbands;
    }

    return 0;
}

static int is_sbc_syncword(const uint8_t *data)
{
    return data[0] == MSBC_SYNCWORD || data[0] == SBC_SYNCWORD;
}

static int sbc_parse_header(AVCodecParserContext *s, AVCodecContext *avctx,
                            const uint8_t *data, size_t len)
{
    static const int sample_rates[4] = { 16000, 32000, 44100, 48000 };
    int sr, blocks, mode, subbands, bitpool, channels, joint;
    int length;

    if (len < 3)
        return -1;

    if (data[0] == MSBC_SYNCWORD && data[1] == 0 && data[2] == 0) {
        av_channel_layout_uninit(&avctx->ch_layout);
        avctx->ch_layout.order       = AV_CHANNEL_ORDER_UNSPEC;
        avctx->ch_layout.nb_channels = 1;
        avctx->sample_rate = 16000;
        avctx->frame_size = 120;
        s->duration = avctx->frame_size;
        return 57;
    }

    if (data[0] != SBC_SYNCWORD)
        return -2;

    sr       =   (data[1] >> 6) & 0x03;
    blocks   = (((data[1] >> 4) & 0x03) + 1) << 2;
    mode     =   (data[1] >> 2) & 0x03;
    subbands = (((data[1] >> 0) & 0x01) + 1) << 2;
    bitpool  = data[2];
    if (bitpool < 2 || bitpool > max_bitpool(mode, subbands))
        return -3;

    channels = mode == SBC_MODE_MONO ? 1 : 2;
    joint    = mode == SBC_MODE_JOINT_STEREO;

    length = 4 + (subbands * channels) / 2
             + ((((mode == SBC_MODE_DUAL_CHANNEL) + 1) * blocks * bitpool
                 + (joint * subbands)) + 7) / 8;

    av_channel_layout_uninit(&avctx->ch_layout);
    avctx->ch_layout.order       = AV_CHANNEL_ORDER_UNSPEC;
    avctx->ch_layout.nb_channels = channels;
    avctx->sample_rate = sample_rates[sr];
    avctx->frame_size = subbands * blocks;
    s->duration = avctx->frame_size;
    return length;
}

static int sbc_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                     const uint8_t **poutbuf, int *poutbuf_size,
                     const uint8_t *buf, int buf_size)
{
    SBCParseContext *sbc = s->priv_data;
    ParseContext *pc = &sbc->pc;
    int left_size = buf_size;
    int i, next;

    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        *poutbuf      = buf;
        *poutbuf_size = buf_size;
        return buf_size;
    }

    if (!pc->frame_start_found) {
        for (i = 0; i < buf_size; i++) {
            memmove(sbc->header, &sbc->header[1], 2);
            sbc->header[2] = buf[i];
            if (!is_sbc_syncword(sbc->header))
                continue;

            sbc->frame_len = sbc_parse_header(s, avctx, sbc->header, 3);
            if (sbc->frame_len > 0) {
                pc->frame_start_found = 1;

                if (i < 2) {
                    int header_size = 2 - i;
                    const uint8_t *header = sbc->header;
                    sbc->buffered_size = header_size;
                    ff_combine_frame(pc, END_NOT_FOUND, &header, &header_size);
                } else {
                    sbc->buffered_size = 0;
                    buf       = &buf[i - 2];
                    left_size = buf_size - i + 2;
                }
                break;
            }
        }

        if (i == buf_size) {
            /* AVERROR_INVALIDDATA */
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }
    }

    next = sbc->frame_len - sbc->buffered_size;
    if (next > left_size) {
        sbc->buffered_size += left_size;
        next = END_NOT_FOUND;
    }

    *poutbuf_size = left_size;
    if (ff_combine_frame(pc, next, &buf, poutbuf_size) < 0) {
        /* NO ENOUGH DATA */
        *poutbuf      = NULL;
        *poutbuf_size = 0;
        return buf_size;
    }

    *poutbuf              = buf;
    pc->frame_start_found = 0;
    memset(sbc->header, 0, sizeof(sbc->header));

    return buf_size - left_size + next;
}

const AVCodecParser ff_sbc_parser = {
    .codec_ids      = { AV_CODEC_ID_SBC },
    .priv_data_size = sizeof(SBCParseContext),
    .parser_parse   = sbc_parse,
    .parser_close   = ff_parse_close,
};
