/*
 * Simplified Abstraction Stream Protocol Demuxer
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
 * Simplified Abstraction Stream Protocol Demuxer
 */

#include "avformat.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"

#include "sasp_common.h"

typedef struct SASPDecContext {
    AVClass *class;
    enum AVCodecID audio_codec_id;
    enum AVCodecID video_codec_id;
    int audio_stream_idx;
    int video_stream_idx;
    uint64_t audio_pos;
    uint64_t video_pos;
    int noheader;
} SASPDecContext;

static int sasp_probe(const AVProbeData *probe)
{
    if (probe->buf_size >= 4 &&
        AV_RB32(probe->buf) == MKBETAG('s', 'a', 's', 'p'))
            return AVPROBE_SCORE_MAX;

    return 0;
}

static void sasp_add_video(AVFormatContext *ic, SASPStreamHeader *stream_header)
{
    SASPDecContext *s = ic->priv_data;
    AVStream *st;
    FFStream *sti;

    st = avformat_new_stream(ic, NULL);
    sti = ffstream(st);

    s->video_codec_id   = stream_header->video_codec_id;
    s->video_stream_idx = st->index;

    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = stream_header->video_codec_id;

    /* the following parameters can also be extracted
     * from the video data in avformat_find_stream_info
     */
    st->r_frame_rate.num = stream_header->fps;
    st->r_frame_rate.den = 1;
    st->avg_frame_rate   = st->r_frame_rate;
    st->codecpar->width  = stream_header->width;
    st->codecpar->height = stream_header->height;

    avpriv_set_pts_info(st, 64, 1, AV_TIME_BASE);

    /* force h264/h265 parsing, in case there are
     * network problems and the pkts maybe lossed
     */
    sti->need_parsing = AVSTREAM_PARSE_FULL_RAW;
}

static void sasp_add_audio(AVFormatContext *ic, SASPStreamHeader *stream_header)
{
    SASPDecContext *s = ic->priv_data;
    AVStream *st;
    FFStream *sti;

    st = avformat_new_stream(ic, NULL);
    sti = ffstream(st);

    s->audio_codec_id   = stream_header->audio_codec_id;
    s->audio_stream_idx = st->index;

    st->codecpar->codec_type  = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id    = stream_header->audio_codec_id;
    st->codecpar->sample_rate = stream_header->sample_rate;

    av_channel_layout_default(&st->codecpar->ch_layout, stream_header->channel);
    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    sti->need_parsing = AVSTREAM_PARSE_FULL_RAW;
}

static int sasp_read_header(AVFormatContext *ic)
{
    SASPStreamHeader stream_header = { 0 };
    SASPDecContext *s = ic->priv_data;
    AVStream *st;
    FFStream *sti;
    int ret;

    if (s->noheader) {
        ic->ctx_flags |= AVFMTCTX_NOHEADER;
        return 0;
    }

    ret = ff_sasp_read_stream_header(ic, &stream_header);
    if (ret < 0)
        return ret;

    avio_skip(ic->pb, stream_header.header_len - ret);

    if (stream_header.video_codec_id)
        sasp_add_video(ic, &stream_header);

    if (stream_header.audio_codec_id)
        sasp_add_audio(ic, &stream_header);

    return ret;
}

static int sasp_read_packet(AVFormatContext *ic, AVPacket *pkt)
{
    SASPStreamHeader stream_header = { 0 };
    SASPFrameHeader frame_header = { 0 };
    SASPDecContext *s = ic->priv_data;
    AVStream *st;
    int ret;

    ret = ff_sasp_read_frame_header(ic, &frame_header);
    if (ret  < 0)
        return ret;

    if (s->noheader) {
        if (frame_header.type == MKBETAG('v', 'i', 'd', 'e')) {
            stream_header.video_codec_id = frame_header.codec_id;
            stream_header.width = frame_header.info.video.width;
            stream_header.height = frame_header.info.video.height;
            stream_header.fps = frame_header.info.video.fps;

            if (s->video_codec_id == AV_CODEC_ID_NONE) {
                sasp_add_video(ic, &stream_header);
            } else if (s->video_codec_id != stream_header.video_codec_id)
                return AVERROR_INVALIDDATA;
        } else {
            stream_header.audio_codec_id = frame_header.codec_id;
            stream_header.sample_rate = frame_header.info.audio.sample_rate;
            stream_header.channel = frame_header.info.audio.channel;

            if (s->audio_codec_id == AV_CODEC_ID_NONE) {
                sasp_add_audio(ic, &stream_header);
            } else if (s->audio_codec_id != stream_header.audio_codec_id)
                return AVERROR_INVALIDDATA;
        }
    }

    avio_skip(ic->pb, frame_header.header_len - ret);

    ret = av_get_packet(ic->pb, pkt, frame_header.body_len);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Error when get packet from sasp stream.\n");
        return ret;
    }

    if (frame_header.codec_id == s->video_codec_id) {
        pkt->pos          = s->video_pos;
        s->video_pos     += frame_header.body_len;
        pkt->stream_index = s->video_stream_idx;
    } else if (frame_header.codec_id == s->audio_codec_id) {
        pkt->pos          = s->audio_pos;
        s->audio_pos     += frame_header.body_len;
        pkt->stream_index = s->audio_stream_idx;
    } else {
        av_log(s, AV_LOG_ERROR, "Unknown id in frame header.\n");
        return AVERROR_INVALIDDATA;
    }

    st = ic->streams[pkt->stream_index];

    pkt->pts = av_rescale(frame_header.timestamp_s * 1000 + frame_header.timestamp_ms, st->time_base.den, 1000);

    av_log(ic, AV_LOG_TRACE, "Sasp stream %s pkt: len %"PRIu32", pts %"PRIu64"s%"PRIu32"ms, seqnum %"PRIu32"\n",
           frame_header.codec_id == s->video_codec_id ? "video" : "audio",
           frame_header.body_len, frame_header.timestamp_s, frame_header.timestamp_ms,
           frame_header.sequence);

    return ret;
}

av_cold static int sasp_init(AVFormatContext *ic)
{
    /* disable any fps probe */
    ic->fps_probe_size = 0;
    /* only allow 0.5s duration to analyze stream */
    ic->max_analyze_duration = AV_TIME_BASE >> 1;
    return 0;
}

#define OFFSET(x) offsetof(SASPDecContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption sasp_options[] = {
    { "noheader", "set no stream header mode for sasp_demuxer", OFFSET(noheader), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, DEC},
    { NULL },
};

static const AVClass sasp_class = {
        .class_name = "Simplified Abstraction Stream Protocol Demuxer",
        .item_name  = av_default_item_name,
        .option     = sasp_options,
        .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_sasp_demuxer = {
        .name           = "sasp",
        .long_name      = NULL_IF_CONFIG_SMALL("Simplified Abstraction Stream Protocol Demuxer"),
        .flags          = AVFMT_TS_DISCONT,
        .flags_internal = FF_FMT_INIT_CLEANUP,
        .priv_data_size = sizeof(SASPDecContext),
        .read_probe     = sasp_probe,
        .read_header    = sasp_read_header,
        .read_packet    = sasp_read_packet,
        .init           = sasp_init,
        .priv_class     = &sasp_class,
        .extensions     = "sasp",
};
