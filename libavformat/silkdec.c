/*
 * Silk Audio Demuxer
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
 * Silk Audio Demuxer
 * #!SILK_V3 + pkt size(2 bytes) + pkt data(raw)
 */

#include "avformat.h"
#include "demux.h"
#include "internal.h"

#define SILK_HEADER_DATA "#!SILK_V3"
#define SILK_HEADER_SIZE 9

static int silk_probe(const AVProbeData *p)
{
    if (!strncmp(p->buf, SILK_HEADER_DATA, SILK_HEADER_SIZE))
        return AVPROBE_SCORE_MAX;

    return 0;
}

static int silk_read_header(AVFormatContext *s)
{
    AVStream *st;
    int ret;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    avio_skip(s->pb, SILK_HEADER_SIZE);

    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_SILK;
    av_channel_layout_default(&st->codecpar->ch_layout, 1);
    st->codecpar->sample_rate = 16000;

    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    return 0;
}

static int silk_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int length = avio_rl16(s->pb);
    return av_get_packet(s->pb, pkt, length);
}

const FFInputFormat ff_silk_demuxer = {
    .p.name       = "silk",
    .p.long_name  = NULL_IF_CONFIG_SMALL("Silk Audio Demuxer"),
    .p.flags      = AVFMT_GENERIC_INDEX,
    .p.extensions = "silk",
    .p.mime_type  = "audio/silk",
    .raw_codec_id = AV_CODEC_ID_SILK,
    .read_probe   = silk_probe,
    .read_header  = silk_read_header,
    .read_packet  = silk_read_packet,
};
