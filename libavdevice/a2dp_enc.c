/*
 * A2DP input and output
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
 * A2DP input and output: output
 *
 * This avdevice decoder can capture audio from an A2DP device.
 *
 * The capture period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time capture.
 */

#include "libavformat/internal.h"
#include "libavformat/avformat.h"
#include "libavutil/time.h"
#include "libavutil/opt.h"
#include "a2dp.h"

#include <poll.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>

static int a2dp_write_header(AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;
    AVStream *st = ctx->streams[0];
    int ret;

    if (ctx->nb_streams != 1 || ctx->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return AVERROR(EINVAL);

    if (ctx->flags & AVFMT_FLAG_NONBLOCK)
        a2dp->nonblock = true;

    a2dp->playback     = true;
    a2dp->sample_rate  = st->codecpar->sample_rate;
    a2dp->channels     = st->codecpar->channels;
    a2dp->codec_id     = st->codecpar->codec_id;
    a2dp->frame_size   = st->codecpar->frame_size;
    a2dp->bit_rate     = st->codecpar->bit_rate;

    ret = ff_a2dp_open(ctx);
    if (ret >= 0)
        avpriv_set_pts_info(st, 64, 1, a2dp->sample_rate);

    return ret;
}

static int a2dp_write_trailer(struct AVFormatContext *ctx)
{
    ff_a2dp_close(ctx);
    return 0;
}

static int a2dp_write_lastpacket(AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;
    int ret;

    ret = ff_a2dp_write_buffer(a2dp, a2dp->lastpkt->data, a2dp->lastpkt->size);
    if (ret < 0)
        return ret;

    a2dp->lastpkt->data += ret;
    a2dp->lastpkt->size -= ret;
    if (a2dp->lastpkt->size)
        return AVERROR(EAGAIN);

    av_packet_free(&a2dp->lastpkt);
    a2dp->available = false;
    return 0;
}

static int a2dp_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    A2dpPriv *a2dp = ctx->priv_data;
    int64_t dts;
    int size;
    int ret;

    if (a2dp->lastpkt)
        return a2dp_write_lastpacket(ctx);

    if (!pkt)
        return 0;

    ret = ff_a2dp_write_buffer(a2dp, pkt->data, pkt->size);
    if (ret < 0)
        return ret;

    if (ret != pkt->size) {
        a2dp->lastpkt = av_packet_clone(pkt);
        a2dp->lastpkt->data += ret;
        a2dp->lastpkt->size -= ret;
        return AVERROR(EAGAIN);
    }

    a2dp->available = false;
    return 0;
}

static int a2dp_write_frame(AVFormatContext *s1, int stream_index,
                             AVFrame **frame, unsigned flags)
{
    A2dpPriv *priv = s1->priv_data;
    AVPacket pkt;

    /* a2dp_enc_open() should have accepted only supported formats */
    if ((flags & AV_WRITE_UNCODED_FRAME_QUERY))
        return av_sample_fmt_is_planar(s1->streams[stream_index]->codecpar->format) ?
               AVERROR(EINVAL) : 0;

    /* set only used fields */
    pkt.data     = (*frame)->data[0];
    pkt.size     = (*frame)->nb_samples * priv->frame_size;
    pkt.dts      = (*frame)->pkt_dts;
    pkt.duration = (*frame)->pkt_duration;
    return a2dp_write_packet(s1, &pkt);
}

static int a2dp_enc_init(struct AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;
    int ret;

    a2dp->data_fd = A2DP_AUDIO_DISCONNECTED;
    a2dp->ctrl_fd = A2DP_AUDIO_DISCONNECTED;
    a2dp->state = AUDIO_A2DP_STATE_STOPPED;
    ret = ff_a2dp_read_output_config(a2dp);

    return ret < 0 ? ret : 1;
}

static void a2dp_enc_deinit(struct AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;
    if (a2dp->lastpkt)
        av_packet_free(&a2dp->lastpkt);

    ff_a2dp_deinit_path(a2dp);
}

static int a2dp_enc_control_message(struct AVFormatContext *ctx, int type,
                                 void *data, size_t data_size)
{
    A2dpPriv *a2dp = ctx->priv_data;
    struct pollfd *poll = data;
    int ret = 0;

    switch (type) {
        case AV_APP_TO_DEV_GET_POLLFD:
            {
                if (!data || data_size < sizeof(struct pollfd) * 2)
                    return AVERROR(EINVAL);

                if (a2dp->ctrl_fd <= 0)
                    return AVERROR(EPERM);

                poll[0].fd      = a2dp->ctrl_fd;
                poll[0].events  = POLLIN;

                if (a2dp->data_fd != A2DP_AUDIO_DISCONNECTED && !a2dp->available) {
                    poll[1].fd      = a2dp->data_fd;
                    poll[1].events  = POLLOUT;
                } else {
                    poll[1].fd      = 0;
                    poll[1].events  = 0;
                }

                poll[2].fd      = 0;
                poll[2].events  = 0;
                break;
            }
        case AV_APP_TO_DEV_POLL_AVAILABLE:
            {
                if (!data || data_size != sizeof(struct pollfd))
                    return AVERROR(EINVAL);

                if (a2dp->ctrl_fd == poll->fd) {
                    ret = ff_a2dp_resp_arrived(a2dp);
                    if (ret > 0 && a2dp->state == AUDIO_A2DP_STATE_STARTING)
                        a2dp->state = AUDIO_A2DP_STATE_STARTED;
                    else
                        break;
                }

                if (a2dp->data_fd == poll->fd && a2dp->data_fd != A2DP_AUDIO_DISCONNECTED)
                    a2dp->available = true;

                avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_BUFFER_WRITABLE, NULL, 0);
                break;
            }
        case AV_APP_TO_DEV_GET_FORMAT_REQUEST:
            {
                AVDictionary **dict = (AVDictionary **)data;
                if (dict != NULL) {
                    av_dict_set_int(dict, "ab", a2dp->bit_rate, 0);
                    av_dict_set_int(dict, "ar", a2dp->sample_rate, 0);
                    av_dict_set_int(dict, "ac", a2dp->channels, 0);
                    return 0;
                }
                break;
            }

    }

    return ret;
}

static int a2dp_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                       const char *key, int flags)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    A2dpPriv *a2dp = devcap->device_context->priv_data;
    struct AVOptionRanges *ranges = av_mallocz(sizeof(struct AVOptionRanges));
    AVOptionRange **range_array = av_mallocz(sizeof(AVOptionRange *));
    AVOptionRange *range = av_mallocz(sizeof(AVOptionRange));
    int ret;

    if (!ranges || !range || !range_array) {
        ret = AVERROR(ENOMEM);
        goto err;
    }

    ranges->range = range_array;
    ranges->range[0] = range;
    ranges->nb_ranges = 1;
    ranges->nb_components = 1;
    range->is_range = 0;

    if (!strcmp(key, "codec")) {
      range->value_min = a2dp->codec_id;
      range->value_max = a2dp->codec_id;
    } else if (!strcmp(key, "channels")) {
        range->value_min = a2dp->channels;
        range->value_max = a2dp->channels;
    } else if (!strcmp(key, "sample_rates")) {
        range->value_min = a2dp->sample_rate;
        range->value_max = a2dp->sample_rate;
    } else {
        ret = AVERROR(EINVAL);
        goto err;
    }

    *ranges_ = ranges;
    return ranges->nb_components;

err:
    av_opt_freep_ranges(&ranges);
    return ret;
}

static const AVClass a2dp_enc_cap_class = {
    .class_name   = "A2DP outdev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges = a2dp_capbility_query_ranges,
};

static int a2dp_create_device_capabilities(struct AVFormatContext *ctx, struct AVDeviceCapabilitiesQuery *caps)
{
    caps->av_class = &a2dp_enc_cap_class;
    return 0;
}

static int a2dp_free_device_capabilities(struct AVFormatContext *ctx, struct AVDeviceCapabilitiesQuery *caps)
{
    return 0;
}

#define OFFSET(x) offsetof(A2dpPriv, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT, {.i64 = 44100}, 1, INT_MAX, FLAGS },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT, {.i64 = 2},     1, INT_MAX, FLAGS },
    { "bit_rate",       "", OFFSET(bit_rate),       AV_OPT_TYPE_INT, {.i64 = 328000}, 1, INT_MAX, FLAGS },
    { "bit_per_sample", "", OFFSET(bit_per_sample), AV_OPT_TYPE_INT, {.i64 = 16}, 1, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass a2dp_muxer_class = {
    .class_name     = "A2DP outdev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
};

AVOutputFormat ff_a2dp_muxer = {
    .name           = "a2dp",
    .long_name      = NULL_IF_CONFIG_SMALL("A2DP audio output"),
    .priv_data_size = sizeof(A2dpPriv),
    .audio_codec    = AV_NE(AV_CODEC_ID_SBC, AV_CODEC_ID_SBC),
    .video_codec    = AV_CODEC_ID_NONE,
    .init           = a2dp_enc_init,
    .deinit         = a2dp_enc_deinit,
    .write_header   = a2dp_write_header,
    .write_packet   = a2dp_write_packet,
    .write_trailer  = a2dp_write_trailer,
    .control_message     = a2dp_enc_control_message,
    .write_uncoded_frame = a2dp_write_frame,
    .create_device_capabilities = a2dp_create_device_capabilities,
    .free_device_capabilities   = a2dp_free_device_capabilities,
    .flags          = AVFMT_NOFILE|AVFMT_TS_NONSTRICT,
    .priv_class     = &a2dp_muxer_class,
};
