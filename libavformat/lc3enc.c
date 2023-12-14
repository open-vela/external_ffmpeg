/*
 * LC3 Audio Muxer
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
 * LC3(Low Complexity Communication Codec) Audio Muxer
 */

#include <lc3.h>

#include "lc3header.h"
#include "avformat.h"
#include "internal.h"

static int lc3_write_header(AVFormatContext *s)
{
    AVStream *st = s->streams[0];
    AVCodecParameters *par = st->codecpar;
    int nchannels = par->ch_layout.nb_channels;
    int pcm_sbits = par->bits_per_raw_sample;
    int srate_hz  = par->sample_rate;
    int bitrate   = par->bit_rate;
    LC3_header hdr;
    int frame_us;
    int nsamples;

    frame_us = AV_TIME_BASE * par->frame_size / srate_hz;
    nsamples = av_rescale_q(st->duration, st->time_base, av_make_q(1, par->sample_rate));

    hdr.file_id        = LC3_FILE_ID,
    hdr.header_size    = sizeof(LC3_header),
    hdr.srate_100hz    = srate_hz / 100,
    hdr.bitrate_100bps = bitrate / 100,
    hdr.channels       = nchannels,
    hdr.frame_10us     = frame_us / 10,
    hdr.rfu            = pcm_sbits;
    hdr.nsamples_low   = nsamples & 0xffff,
    hdr.nsamples_high  = nsamples >> 16,

    avio_write(s->pb, (void*)&hdr, sizeof(LC3_header));

    return 0;
}

static int lc3_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    if (!pkt->size)
      return 0;

    avio_wl16(s->pb, pkt->size);
    avio_write(s->pb, pkt->data, pkt->size);

    return 0;
}

AVOutputFormat ff_lc3_muxer = {
    .name         = "lc3",
    .long_name    = NULL_IF_CONFIG_SMALL("LC3 Audio Muxer"),
    .extensions   = "lc3",
    .mime_type    = "audio/lc3",
    .audio_codec  = AV_CODEC_ID_LC3,
    .video_codec  = AV_CODEC_ID_NONE,
    .write_header = lc3_write_header,
    .write_packet = lc3_write_packet,
    .flags        = AVFMT_NOTIMESTAMPS,
};
