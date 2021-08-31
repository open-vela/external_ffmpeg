/*
 * FLUORIDE input and output
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
 * FLUORIDE input and output: output
 *
 * This avdevice decoder can capture audio from an FLUORIDE device.
 *
 * The capture period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time capture.
 */

#include "libavformat/internal.h"
#include "libavformat/avformat.h"
#include "libavutil/time.h"
#include "libavutil/opt.h"
#include "fluoride.h"

#include <poll.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>

static int fluoride_write_header(AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;
    AVStream *st = ctx->streams[0];
    int ret;

    if (ctx->nb_streams != 1 ||
            ctx->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return AVERROR(EINVAL);

    if (ctx->flags & AVFMT_FLAG_NONBLOCK)
        priv->nonblock = true;

    priv->playback     = true;
    priv->sample_rate  = st->codecpar->sample_rate;
    priv->channels     = st->codecpar->channels;
    priv->codec_id     = st->codecpar->codec_id;
    priv->frame_size   = st->codecpar->frame_size;
    priv->bit_rate     = st->codecpar->bit_rate;

    ret = ff_fluoride_open(ctx);
    if (ret >= 0)
        avpriv_set_pts_info(st, 64, 1, priv->sample_rate);

    return ret;
}

static int fluoride_write_trailer(struct AVFormatContext *ctx)
{
    ff_fluoride_close(ctx);
    return 0;
}

static int fluoride_write_lastpacket(AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;
    int ret;

    ret = ff_fluoride_write_buffer(priv, priv->lastpkt->data, priv->lastpkt->size);
    if (ret < 0)
        return ret;

    priv->lastpkt->data += ret;
    priv->lastpkt->size -= ret;
    if (priv->lastpkt->size)
        return AVERROR(EAGAIN);

    av_packet_free(&priv->lastpkt);
    priv->available = false;
    return 0;
}

static int fluoride_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    FluPriv *priv = ctx->priv_data;
    int64_t dts;
    int size;
    int ret;

    if (priv->lastpkt)
        return fluoride_write_lastpacket(ctx);

    if (!pkt)
        return 0;

    ret = ff_fluoride_write_buffer(priv, pkt->data, pkt->size);
    if (ret < 0)
        return ret;

    if (ret != pkt->size) {
        priv->lastpkt = av_packet_clone(pkt);
        priv->lastpkt->data += ret;
        priv->lastpkt->size -= ret;
        return AVERROR(EAGAIN);
    }

    priv->available = false;
    return 0;
}

static int fluoride_write_frame(AVFormatContext *s1, int stream_index,
                             AVFrame **frame, unsigned flags)
{
    FluPriv *priv = s1->priv_data;
    AVPacket pkt;

    /* fluoride_enc_open() should have accepted only supported formats */
    if ((flags & AV_WRITE_UNCODED_FRAME_QUERY))
        return av_sample_fmt_is_planar(
                s1->streams[stream_index]->codecpar->format) ?  AVERROR(EINVAL) : 0;

    /* set only used fields */
    pkt.data     = (*frame)->data[0];
    pkt.size     = (*frame)->nb_samples * priv->frame_size;
    pkt.dts      = (*frame)->pkt_dts;
    pkt.duration = (*frame)->pkt_duration;
    return fluoride_write_packet(s1, &pkt);
}

static int fluoride_enc_init(struct AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;
    int ret;

    priv->data_fd = FLUORIDE_AUDIO_DISCONNECTED;
    priv->ctrl_fd = FLUORIDE_AUDIO_DISCONNECTED;
    priv->state = FLUORIDE_STATE_STOPPED;
    ret = ff_fluoride_read_output_config(priv);

    return ret < 0 ? ret : 1;
}

static void fluoride_enc_deinit(struct AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;
    if (priv->lastpkt)
        av_packet_free(&priv->lastpkt);

    ff_fluoride_deinit_path(priv);
}

static int fluoride_enc_control_message(struct AVFormatContext *ctx, int type,
                                 void *data, size_t data_size)
{
    FluPriv *priv = ctx->priv_data;
    struct pollfd *poll = data;
    int ret = 0;

    switch (type) {
        case AV_APP_TO_DEV_GET_POLLFD:
            {
                if (!data || data_size < sizeof(struct pollfd) * 2)
                    return AVERROR(EINVAL);

                if (priv->ctrl_fd <= 0)
                    return AVERROR(EPERM);

                poll[0].fd      = priv->ctrl_fd;
                poll[0].events  = POLLIN;

                if (priv->data_fd != FLUORIDE_AUDIO_DISCONNECTED && !priv->available) {
                    poll[1].fd      = priv->data_fd;
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

                if (priv->ctrl_fd == poll->fd) {
                    ret = ff_fluoride_resp_arrived(priv);
                    if (ret > 0 && priv->state == FLUORIDE_STATE_STARTING)
                        priv->state = FLUORIDE_STATE_STARTED;
                    else
                        break;
                }

                if (priv->data_fd == poll->fd && priv->data_fd != FLUORIDE_AUDIO_DISCONNECTED)
                    priv->available = true;

                avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_BUFFER_WRITABLE, NULL, 0);
                break;
            }
        case AV_APP_TO_DEV_GET_FORMAT_REQUEST:
            {
                AVDictionary **dict = (AVDictionary **)data;
                if (dict != NULL) {
                    av_dict_set_int(dict, "ab", priv->bit_rate, 0);
                    av_dict_set_int(dict, "ar", priv->sample_rate, 0);
                    av_dict_set_int(dict, "ac", priv->channels, 0);
                    return 0;
                }
                break;
            }

    }

    return ret;
}

static int fluoride_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                       const char *key, int flags)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    FluPriv *priv = devcap->device_context->priv_data;
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
      range->value_min = priv->codec_id;
      range->value_max = priv->codec_id;
    } else if (!strcmp(key, "channels")) {
        range->value_min = priv->channels;
        range->value_max = priv->channels;
    } else if (!strcmp(key, "sample_rates")) {
        range->value_min = priv->sample_rate;
        range->value_max = priv->sample_rate;
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

static const AVClass fluoride_enc_cap_class = {
    .class_name   = "FLUORIDE outdev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges = fluoride_capbility_query_ranges,
};

static int fluoride_create_device_capabilities(struct AVFormatContext *ctx, struct AVDeviceCapabilitiesQuery *caps)
{
    caps->av_class = &fluoride_enc_cap_class;
    return 0;
}

static int fluoride_free_device_capabilities(struct AVFormatContext *ctx, struct AVDeviceCapabilitiesQuery *caps)
{
    return 0;
}

#define OFFSET(x) offsetof(FluPriv, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "sample_rate",    "", OFFSET(sample_rate),    AV_OPT_TYPE_INT, {.i64 = 44100}, 1, INT_MAX, FLAGS },
    { "channels",       "", OFFSET(channels),       AV_OPT_TYPE_INT, {.i64 = 2},     1, INT_MAX, FLAGS },
    { "bit_rate",       "", OFFSET(bit_rate),       AV_OPT_TYPE_INT, {.i64 = 328000}, 1, INT_MAX, FLAGS },
    { "bit_per_sample", "", OFFSET(bit_per_sample), AV_OPT_TYPE_INT, {.i64 = 16}, 1, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass fluoride_muxer_class = {
    .class_name     = "FLUORIDE outdev",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
    .category       = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
};

AVOutputFormat ff_fluoride_muxer = {
    .name           = "fluoride",
    .long_name      = NULL_IF_CONFIG_SMALL("FLUORIDE audio output"),
    .priv_data_size = sizeof(FluPriv),
    .audio_codec    = AV_NE(AV_CODEC_ID_SBC, AV_CODEC_ID_SBC),
    .video_codec    = AV_CODEC_ID_NONE,
    .init           = fluoride_enc_init,
    .deinit         = fluoride_enc_deinit,
    .write_header   = fluoride_write_header,
    .write_packet   = fluoride_write_packet,
    .write_trailer  = fluoride_write_trailer,
    .control_message     = fluoride_enc_control_message,
    .write_uncoded_frame = fluoride_write_frame,
    .create_device_capabilities = fluoride_create_device_capabilities,
    .free_device_capabilities   = fluoride_free_device_capabilities,
    .flags          = AVFMT_NOFILE|AVFMT_TS_NONSTRICT,
    .priv_class     = &fluoride_muxer_class,
};
