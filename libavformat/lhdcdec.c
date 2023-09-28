/*
 * LHDC Audio Demuxer
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
 * LHDC Audio Demuxer
 */

#include "avformat.h"
#include "internal.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "rawdec.h"

#define LHDC4_FILE_HEAD_LEN (8)
#define LHDC5_FILE_HEAD_LEN (28)

typedef struct LHDCContext {
    const AVClass *class;
    int packet_size;
    char *version;
} LHDCContext;

static const uint8_t lhdc_header[4] = "LHDC";

static int lhdc_probe(const AVProbeData *p)
{
    if (!memcmp(p->buf, lhdc_header, 4))
        return AVPROBE_SCORE_MAX;

    return 0;
}

static int parse_header_v4(AVFormatContext *s, enum AVCodecID codec_id)
{
    LHDCContext *lhdc = s->priv_data;
    uint8_t header[LHDC4_FILE_HEAD_LEN];
    AVStream *st;
    int bits_per_sample;
    int sample_rate;
    int frame_size;
    int read;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    read = avio_read(s->pb, header, sizeof(header));
    if (read < 0)
        return read;

    if (!memcmp(header, lhdc_header, sizeof(lhdc_header))) {
        if (header[5] == 44)
            sample_rate = 44100;
        else
            sample_rate = (unsigned int)header[5] * 1000;

        bits_per_sample = header[6];
        frame_size      = 1 << header[7];
    } else {
        return AVERROR_INVALIDDATA;
    }

    st->codecpar->format              = AV_SAMPLE_FMT_S32;
    st->codecpar->bits_per_raw_sample = bits_per_sample;
    st->codecpar->codec_type          = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id            = codec_id;
    st->codecpar->sample_rate         = sample_rate;
    st->codecpar->frame_size          = frame_size;

    if (bits_per_sample == 16)
        st->codecpar->format = AV_SAMPLE_FMT_S16;

    ffstream(st)->need_parsing = AVSTREAM_PARSE_FULL_RAW;

    av_channel_layout_default(&st->codecpar->ch_layout, 2);
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    return 0;

}

static int parse_header_v5(AVFormatContext *s, enum AVCodecID codec_id)
{
    uint8_t header[LHDC5_FILE_HEAD_LEN];
    AVStream *st;
    int av_unused total_samples;
    int av_unused enc_len;
    int bits_per_sample;
    int sample_rate;
    int frame_size;
    int channels;
    int read;

    st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    read = avio_read(s->pb, header, sizeof(header));
    if (read < 0)
        return read;

    if (!memcmp(header, lhdc_header, sizeof(lhdc_header))) {
        channels        = AV_RL32(header + 4);
        sample_rate     = AV_RL32(header + 8);
        bits_per_sample = AV_RL32(header + 12);
        frame_size      = AV_RL32(header + 16);
        enc_len         = AV_RL32(header + 20);
        total_samples   = AV_RL32(header + 24);
    } else {
        return AVERROR_INVALIDDATA;
    }

    st->codecpar->format              = AV_SAMPLE_FMT_S32;
    st->codecpar->bits_per_raw_sample = bits_per_sample;
    st->codecpar->codec_type          = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id            = codec_id;
    st->codecpar->sample_rate         = sample_rate;
    st->codecpar->frame_size          = frame_size;

    if (bits_per_sample == 16)
        st->codecpar->format = AV_SAMPLE_FMT_S16;

    ffstream(st)->need_parsing = AVSTREAM_PARSE_FULL_RAW;

    av_channel_layout_default(&st->codecpar->ch_layout, channels);
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    return 0;

}

static int lhdc_read_header(AVFormatContext *s)
{
    LHDCContext *lhdc = s->priv_data;
    if (!memcmp(lhdc->version, "lhdc4", sizeof("lhdc4")))
        return parse_header_v4(s, AV_CODEC_ID_LHDC4);
    else if (!memcmp(lhdc->version, "lhdc3", sizeof("lhdc3")))
        return parse_header_v4(s, AV_CODEC_ID_LHDC3);
    else if (!memcmp(lhdc->version, "llac", sizeof("llac")))
        return parse_header_v4(s, AV_CODEC_ID_LLAC);
    else if (!memcmp(lhdc->version, "lhdc5", sizeof("lhdc5")))
        return parse_header_v5(s, AV_CODEC_ID_LHDC5);
    else
        av_log(s, AV_LOG_ERROR, "Unsupported lhdc version %s!\n", lhdc->version);

    return AVERROR(EINVAL);
}

static int lhdc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    LHDCContext *ctx = s->priv_data;
    int ret, size;

    size = ctx->packet_size;

    if ((ret = av_new_packet(pkt, size)) < 0)
        return ret;

    pkt->pos = avio_tell(s->pb);
    pkt->stream_index = 0;
    ret = avio_read_partial(s->pb, pkt->data, size);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }
    av_shrink_packet(pkt, ret);
    return ret;
}

static const AVOption lhdc_options[] = {
    {"packet_size", "packet size", offsetof(LHDCContext, packet_size), AV_OPT_TYPE_INT,    {.i64 = 1024 },   1, INT_MAX, AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM},
    {"version",     "lhdc version",     offsetof(LHDCContext, version),     AV_OPT_TYPE_STRING, {.str = "lhdc4"}, 0, 0,       AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_DECODING_PARAM},
    {NULL},
};

static const AVClass lhdc_class = {
    .class_name = "lhdc",
    .item_name  = av_default_item_name,
    .option     = lhdc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_lhdc_demuxer = {
    .name           = "lhdc",
    .long_name      = NULL_IF_CONFIG_SMALL("LHDC Audio Demuxer"),
    .priv_class     = &lhdc_class,
    .priv_data_size = sizeof(LHDCContext),
    .read_probe     = lhdc_probe,
    .read_header    = lhdc_read_header,
    .read_packet    = lhdc_read_packet,
    .flags          = AVFMT_GENERIC_INDEX,
    .extensions     = "lhdc",
    .mime_type      = "audio/lhdc",
};
