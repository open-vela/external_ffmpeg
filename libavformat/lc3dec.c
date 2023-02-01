/*
 * LC3 Audio Demuxer
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
 * LC3(Low Complexity Communication Codec) Audio Demuxer
 */

#include <lc3.h>

#include "lc3header.h"
#include "avformat.h"
#include "internal.h"

static int lc3_probe(const AVProbeData *p)
{
    if ((p->buf[0] | p->buf[1] << 8) == LC3_FILE_ID)
        return AVPROBE_SCORE_MAX;

    return 0;
}

static int lc3_read_header(AVFormatContext *s)
{
    int ret, frame_us;
    AVStream *st;
    LC3_header hdr;

    st = avformat_new_stream(s, NULL);
    if (!st) {
        return AVERROR(ENOMEM);
    }

    ret = avio_read(s->pb, (void*)&hdr, sizeof(LC3_header));
    if (ret != sizeof(LC3_header)) {
        av_log(s, AV_LOG_ERROR, "Read LC3 header failed!\n");
        return AVERROR_INVALIDDATA;
    }

    frame_us = hdr.frame_10us * 10;
    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = AV_CODEC_ID_LC3;
    st->codecpar->sample_rate = hdr.srate_100hz * 100;
    st->codecpar->format      = AV_SAMPLE_FMT_S16;
    st->codecpar->frame_size  = st->codecpar->sample_rate * frame_us / AV_TIME_BASE;
    st->codecpar->ch_layout.nb_channels = hdr.channels;
    st->nb_frames = hdr.nsamples_low | (hdr.nsamples_high << 16);

    av_channel_layout_default(&st->codecpar->ch_layout, hdr.channels);
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    return 0;
}

static int lc3_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st = s->streams[0];
    AVCodecParameters *par = st->codecpar;
    int nch = par->ch_layout.nb_channels;
    uint16_t nbytes;

    if (avio_feof(s->pb))
        return AVERROR_EOF;

    avio_read(s->pb, (void*)&nbytes, sizeof(uint16_t));
    if (nbytes > nch * LC3_MAX_FRAME_BYTES
        || nbytes % nch) {
        return AVERROR_EOF;
    }

    return av_get_packet(s->pb, pkt, nbytes);
}

AVInputFormat ff_lc3_demuxer = {
    .name         = "lc3",
    .long_name    = NULL_IF_CONFIG_SMALL("LC3 Audio Demuxer"),
    .read_probe   = lc3_probe,
    .read_header  = lc3_read_header,
    .read_packet  = lc3_read_packet,
    .flags        = AVFMT_GENERIC_INDEX,
    .extensions   = "lc3",
    .mime_type    = "audio/lc3",
    .raw_codec_id = AV_CODEC_ID_LC3,
};
