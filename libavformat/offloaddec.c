/*
 * Audio Offload demuxer
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

#include "avformat.h"
#include "internal.h"
#include "libavutil/opt.h"

typedef struct OffloadDemuxerContext {
    const AVClass *class; /**< Class for private options. */
    AVStream *st;
    char *codec_name;
    int idx;
    int sample_rate;
    int packet_size;
    AVChannelLayout ch_layout;
} OffloadDemuxerContext;

struct offload_codec_table {
    const char *codec_name; /**< android codec name */
    int codec_id;           /**< ffmepg codec id*/
    int parse_type;
    int packet_size;
};

static const struct offload_codec_table codec_tables[] = {
    { "AUDIO_FORMAT_MP3", AV_CODEC_ID_MP3, AVSTREAM_PARSE_FULL_RAW, 1024 },
    { "AUDIO_FORMAT_PCM_16_BIT", AV_CODEC_ID_PCM_S16LE, AVSTREAM_PARSE_NONE, 1024 },
    { "AUDIO_FORMAT_NONE", AV_CODEC_ID_NONE, AVSTREAM_PARSE_NONE, 0 }
};

static int offload_get_codec_idx(const char *codec_name)
{
    int i;
    if (!codec_name)
        return AVERROR(EINVAL);
    for (i = 0; i < FF_ARRAY_ELEMS(codec_tables); i++)
        if (!strcmp(codec_name, codec_tables[i].codec_name))
            return i;

    av_log(NULL, AV_LOG_ERROR, "unknown codec name %s.", codec_name);

    return AVERROR(EINVAL);
}

static int offload_probe(const AVProbeData *p)
{
    return 0;
}

static int offload_read_header(AVFormatContext *s)
{
    AVStream *st;
    FFStream *sti;
    OffloadDemuxerContext *s1 = s->priv_data;

    av_log(s, AV_LOG_INFO, "%s codec_name:%s, rate:%d, channels:%d.", __func__,
           s1->codec_name, s1->sample_rate, s1->ch_layout.nb_channels);

    if ((s1->idx = offload_get_codec_idx(s1->codec_name)) < 0)
        return AVERROR(EINVAL);

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    s1->st                    = st;
    sti                       = ffstream(st);
    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->ch_layout   = s1->ch_layout;
    st->codecpar->sample_rate = s1->sample_rate;
    st->codecpar->codec_id    = codec_tables[s1->idx].codec_id;
    sti->need_parsing         = codec_tables[s1->idx].parse_type;
    s1->packet_size           = codec_tables[s1->idx].packet_size;

    av_log(s, AV_LOG_INFO, "%s codec_id:%d need_parsing:%d\n", __func__,
           st->codecpar->codec_id, sti->need_parsing);

    return 0;
}

static int offload_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    OffloadDemuxerContext *s1 = s->priv_data;
    int ret;
    ret = av_get_packet(s->pb, pkt, s1->packet_size);
    if (ret <= 0) {
        if (ret < 0)
            return ret;
        return AVERROR_EOF;
    }

    pkt->flags &= ~AV_PKT_FLAG_CORRUPT;

    return ret;
}

#define OFFSET(x) offsetof(OffloadDemuxerContext, x)

#define D AV_OPT_FLAG_DECODING_PARAM

static const AVOption options[] = {
    { "codec_name", "codec name in string", OFFSET(codec_name), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, D },
    { "sample_rate", "sample rate in Hz", OFFSET(sample_rate), AV_OPT_TYPE_INT, { .i64 = 44100 }, 1, INT_MAX, D },
    { "ch_layout", "ch_layout", OFFSET(ch_layout), AV_OPT_TYPE_CHLAYOUT, { .str = NULL }, 0, 0, D },
    { NULL },
};

static const AVClass offload_class = {
    .class_name = "audio offload demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const AVInputFormat ff_offload_demuxer = {
    .name           = "offload",
    .long_name      = NULL_IF_CONFIG_SMALL("audio for offload"),
    .read_probe     = offload_probe,
    .read_header    = offload_read_header,
    .read_packet    = offload_read_packet,
    .flags          = AVFMT_GENERIC_INDEX,
    .extensions     = "offload",
    .mime_type      = "audio/offload",
    .priv_class     = &offload_class,
    .priv_data_size = sizeof(OffloadDemuxerContext),
    .raw_codec_id   = AV_CODEC_ID_RAWAUDIO,
};
