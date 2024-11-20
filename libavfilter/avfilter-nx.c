/*
 * filter layer
 * Copyright (c) 2007 Bobby Bingham
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
#include "avfilter-nx.h"
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
#include <libavformat/avformat_internal.h>
#include <libavformat/mux.h>

#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"

//libavformat mux.h externs
int write_packets_from_bsfs(AVFormatContext *s, AVStream *st, AVPacket *pkt, int interleaved);
int interleaved_write_packet(AVFormatContext *s, AVPacket *pkt,
                                    int flush, int has_packet);

int av_opt_query_ranges2(AVOptionRanges **ranges_arg, void *obj, void *udata, const char *key, int flags)
{
    int ret;
    const AVClass *c = *(AVClass **)obj;
    int (*callback)(AVOptionRanges **, void *obj, const char *key, int flags) = c->query_ranges;

    if (!callback)
        callback = av_opt_query_ranges_default;

    ret = callback(ranges_arg, udata, key, flags);
    if (ret >= 0) {
        if (!(flags & AV_OPT_MULTI_COMPONENT_RANGE))
            ret = 1;
        (*ranges_arg)->nb_components = ret;
    }
    return ret;
}


int avformat_write_trailer(AVFormatContext *s)
{
    FFFormatContext *const si = ffformatcontext(s);
    AVPacket *const pkt = si->parse_pkt;
    int ret1, ret = 0;

    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *const st = s->streams[i];
        FFStream *const sti = ffstream(st);
        if (sti->bsfc) {
            ret1 = write_packets_from_bsfs(s, st, pkt, 1 /*interleaved*/);
            if (ret1 < 0)
                av_packet_unref(pkt);
            if (ret >= 0)
                ret = ret1;
        }
    }
    ret1 = interleaved_write_packet(s, pkt, 1, 0);
    if (ret >= 0)
        ret = ret1;

    if (ffofmt(s->oformat)->write_trailer) {
        if (!(s->oformat->flags & AVFMT_NOFILE) && s->pb)
            avio_write_marker(s->pb, AV_NOPTS_VALUE, AVIO_DATA_MARKER_TRAILER);
        ret1 = ffofmt(s->oformat)->write_trailer(s);
        if (ret >= 0)
            ret = ret1;
    }

    return ret;
}
