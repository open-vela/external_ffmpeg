/*
 * Silk Audio Muxer
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
 * Silk Audio Muxer
 * #!SILK_V3 + pkt size(2 bytes) + pkt data(raw) + pkt size(2 bytes) + pkt data ...
 */

#include "avformat.h"
#include "mux.h"

static int silk_write_header(AVFormatContext *s)
{
    avio_write(s->pb, "#!SILK_V3", strlen("#!SILK_V3"));

    return 0;
}

static int silk_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;

    if (!pkt->size)
        return 0;

    avio_wl16(pb, pkt->size);
    avio_write(pb, pkt->data, pkt->size);

    return 0;
}

const FFOutputFormat ff_silk_muxer = {
    .p.name         = "silk",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Silk Audio Muxer"),
    .p.mime_type    = "audio/silk",
    .p.extensions   = "silk",
    .p.audio_codec  = AV_CODEC_ID_SILK,
    .p.video_codec  = AV_CODEC_ID_NONE,
    .p.flags        = AVFMT_NOTIMESTAMPS,
    .write_header   = silk_write_header,
    .write_packet   = silk_write_packet,
};
