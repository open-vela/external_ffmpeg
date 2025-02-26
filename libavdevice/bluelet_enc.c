/*
 * Bluelet input and output
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
 * Bluelet input and output: output
 *
 * This avdevice decoder can capture audio from an Bluelet device.
 *
 * The capture period is set to the lower value available for the device,
 * which gives a low latency suitable for real-time capture.
 */

#include "bluelet.h"
#include "libavfilter/filters.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavformat/mux.h"
#include "libavcodec/get_bits.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>

static const AVClass bluelet_enc_cap_class;

static int bluelet_enc_init(struct AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;

    priv->playback = true;
    priv->recv_ts = 0;
    priv->send_ts = 0;
    ff_bluelet_init(priv, !!(ctx->flags & AVFMT_FLAG_NONBLOCK));

    return 1;
}

static void bluelet_enc_deinit(struct AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;

    if (priv->lastpkt)
        av_packet_free(&priv->lastpkt);

    ff_bluelet_deinit(priv);
}

static int bluelet_write_header(AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;
    AVStream *st = ctx->streams[0];
    int ret;

    if (ctx->nb_streams != 1 || ctx->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return AVERROR(EINVAL);

    priv->recv_ts = 0;
    priv->send_ts = 0;

    ret = ff_bluelet_start(priv);
    if (ret >= 0)
        avpriv_set_pts_info(st, 64, 1, priv->sample_rate);

    return ret;
}

static int bluelet_write_trailer(struct AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;

    priv->recv_ts = 0;
    priv->send_ts = 0;

    return ff_bluelet_stop(priv);
}

static int bluelet_write_lastpacket(AVFormatContext *ctx)
{
    BlueletPriv *priv = ctx->priv_data;
    int ret;

    ret = ff_bluelet_write_buffer(priv, priv->lastpkt->data, priv->lastpkt->size);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN))
            av_packet_free(&priv->lastpkt);
        return ret;
    }

    priv->lastpkt->data += ret;
    priv->lastpkt->size -= ret;
    if (priv->lastpkt->size)
        return AVERROR(EAGAIN);

    av_packet_free(&priv->lastpkt);

    return 0;
}

static int bluelet_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    BlueletPriv *priv = ctx->priv_data;
    int64_t ts_now;
    int ret;

    if (priv->lastpkt)
        return bluelet_write_lastpacket(ctx);

    if (!pkt || !pkt->size)
        return 0;

    ts_now = av_gettime_relative();
    if (priv->send_ts > 0) {
        int64_t diff = ts_now - priv->send_ts;

        if (diff > 500000 /* us */)
            av_log(ctx, AV_LOG_WARNING, "send bluelet packet size %d duration %" PRId64 " us\n", pkt->size, diff);
    }
    priv->send_ts = ts_now;
    ret = ff_bluelet_write_buffer(priv, pkt->data, pkt->size);
    if (ret < 0)
        return ret;

    if (ret != pkt->size) {
        priv->lastpkt = av_packet_clone(pkt);
        priv->lastpkt->data += ret;
        priv->lastpkt->size -= ret;
        return AVERROR(EAGAIN);
    }

    return 0;
}

static int bluelet_write_frame(AVFormatContext *s1, int stream_index, AVFrame **frame, unsigned flags)
{
    BlueletPriv *priv = s1->priv_data;
    AVPacket pkt;

    /* bluelet_enc_open() should have accepted only supported formats */
    if ((flags & AV_WRITE_UNCODED_FRAME_QUERY))
        return av_sample_fmt_is_planar(s1->streams[stream_index]->codecpar->format) ? AVERROR(EINVAL) : 0;

    /* set only used fields */
    pkt.data = (*frame)->data[0];
    pkt.size = (*frame)->nb_samples * av_get_bytes_per_sample(s1->streams[stream_index]->codecpar->format);
    pkt.dts = (*frame)->pkt_dts;
    pkt.duration = (*frame)->duration;
    return bluelet_write_packet(s1, &pkt);
}

static int bluelet_enc_control_message(struct AVFormatContext *ctx, int type,
                                       void *data, size_t data_size)
{
    BlueletPriv *priv = ctx->priv_data;
    struct pollfd *poll = data;
    int ret = 0;

    switch (type) {
        case AV_APP_TO_DEV_GET_CAPS_REQUEST: {
            struct AVDeviceCapabilitiesQuery *caps = data;

            if (priv->codec_id == AV_CODEC_ID_NONE)
                return FFERROR_NOT_READY;

            caps->av_class = &bluelet_enc_cap_class;
            caps->device_context = ctx;
            return 0;
        }
        case AV_APP_TO_DEV_GET_POLLFD: {
            if (!data || data_size < sizeof(struct pollfd) * 2)
                return AVERROR(EINVAL);

            if (priv->ctrl_fd > 0) {
                poll[ret].fd = priv->ctrl_fd;
                poll[ret].events = priv->ctrl_connected ? POLLIN : POLLOUT;;
                ret++;
            }
            if (priv->data_fd > 0 && priv->lastpkt != NULL) {
                poll[ret].fd = priv->data_fd;
                poll[ret].events = POLLOUT;
                ret++;
            }
#ifdef CONFIG_UORB
            if (priv->uorb_fd > 0) {
                poll[ret].fd     = priv->uorb_fd;
                poll[ret].events = POLLIN;
                ret++;
            }
#endif
            break;
        }
        case AV_APP_TO_DEV_POLL_AVAILABLE: {
            if (poll->revents & (POLLERR | POLLHUP)) {
                ff_bluelet_disconnect(priv);
                return 0;
            }

            if (!data || data_size != sizeof(struct pollfd))
                return AVERROR(EINVAL);

            if (priv->ctrl_fd == poll->fd) {
                if (poll->revents & POLLOUT)
                    priv->ctrl_connected = true;

                if (poll->revents & POLLIN) {
                    int action = ff_bluelet_handle_event(priv);
                    if (action == BLUELET_ACTION_AVAILABLE) {
                        avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_BUFFER_WRITABLE, NULL, 0);
                    } else if (action == BLUELET_ACTION_CONFIG) {
                        ctx->audio_codec_id = priv->codec_id;
                        avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_STATE_CHANGED, &type, 0);
                    }
                }
            } else if (priv->data_fd == poll->fd) {
                if (priv->lastpkt) {
                    int64_t ts_now = av_gettime_relative();
                    if (priv->recv_ts > 0) {
                        int64_t diff = ts_now - priv->recv_ts;

                        if (diff > 500000 /* us */)
                            av_log(ctx, AV_LOG_WARNING, "recv bluelet poll available duration %" PRId64 " us\n", diff);
                    }
                    priv->recv_ts = ts_now;

                    avdevice_dev_to_app_control_message(ctx, AV_DEV_TO_APP_BUFFER_WRITABLE, NULL, 0);
                }
            }
#ifdef CONFIG_UORB
            else if (priv->uorb_fd == poll->fd)
                ff_bluelet_handle_uorb_event(priv);
#endif
            break;
        }
        case AV_APP_TO_DEV_GET_FORMAT_REQUEST: {
            AVDictionary **dict = (AVDictionary**)data;
            char ch_layout[128];

            if (dict != NULL) {
                av_channel_layout_describe(&priv->ch_layout, ch_layout, sizeof(ch_layout));
                av_dict_set(dict, "ch_layout", ch_layout, 0);
                av_dict_set_int(dict, "ab", priv->bit_rate, 0);
                if (priv->codec_id == AV_CODEC_ID_SBC) {
                    av_dict_set(dict, "sbc_param", priv->sbc.param, 0);
                    av_dict_set_int(dict, "nb_out_pkts", priv->sbc.nb_out_pkts, 0);
                } else if (priv->codec_id == AV_CODEC_ID_AAC) {
                    av_dict_set_int(dict, "profile", priv->aac.profile, 0);
                    av_dict_set_int(dict, "vbr", priv->aac.vbr, 0);
                    av_dict_set_int(dict, "latm", 1, 0);
                    av_dict_set_int(dict, "peak", 1, 0);
                }
                return 0;
            }
            break;
        }
        case AV_APP_TO_DEV_DUMP: {
            snprintf(data, data_size, "%s|%s|%d|%p|%d,%d",
                     priv->server_name, priv->mode, priv->state, priv->lastpkt, priv->ctrl_fd, priv->data_fd);
            break;
        }
        default:
            ret = AVERROR(ENOSYS);
            break;
    }

    return ret;
}

static int bluelet_enc_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                              const char *key, int flags)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    BlueletPriv *priv = devcap->device_context->priv_data;
    const AVCodec *codec;

    codec = avcodec_find_encoder(priv->codec_id);
    if (!codec)
        return AVERROR(EINVAL);

    return ff_bluelet_capbility_query_ranges(ranges_, obj, codec, key, flags);
}

static const AVClass bluelet_enc_cap_class = {
    .class_name   = "BLUELET outdev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges = bluelet_enc_capbility_query_ranges,
};

static int bluelet_enc_check_bitstream(struct AVFormatContext *ctx, struct AVStream *st,
                                       const AVPacket *pkt)
{
    int ret = 1;

    if (st->codecpar->codec_id == AV_CODEC_ID_AAC) {
        /* check aac header, if loas header is present, skip add bitstream filter */
        if(pkt->size > 2 && pkt->data[0] == 0x56 && (pkt->data[1] >> 4) == 0xe &&
           (AV_RB16(pkt->data + 1) & 0x1FFF) + 3 == pkt->size)
            return ret;
        ret = ff_stream_add_bitstream_filter(st, "aac_rawtolatm", NULL);
    }

    return ret;
}

#define OFFSET(x) offsetof(BlueletPriv, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM | AV_OPT_FLAG_AUDIO_PARAM
static const AVOption options[] = {
    { "server_name", "Set server name", OFFSET(server_name), AV_OPT_TYPE_STRING, { .str = "local" }, 0, 0, FLAGS },
    { "mode", "Audio mode", OFFSET(mode), AV_OPT_TYPE_STRING, { .str = "a2dp" }, 0, 0,       FLAGS },
    { "nb_out_pkts", "set out packets num", OFFSET(sbc.nb_out_pkts), AV_OPT_TYPE_INT, {.i64 = 1}, 1, 32, FLAGS },
    { NULL },
};

static const AVClass bluelet_muxer_class = {
    .class_name = "BLUELET outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
};


const FFOutputFormat ff_bluelet_muxer = {
    .p.name                     = "bluelet",
    .p.long_name                = NULL_IF_CONFIG_SMALL("BLUELET audio output"),
    .priv_data_size             = sizeof(BlueletPriv),
    .p.audio_codec              = AV_CODEC_ID_NONE,
    .p.video_codec              = AV_CODEC_ID_NONE,
    .init                       = bluelet_enc_init,
    .deinit                     = bluelet_enc_deinit,
    .write_header               = bluelet_write_header,
    .write_packet               = bluelet_write_packet,
    .write_trailer              = bluelet_write_trailer,
    .control_message            = bluelet_enc_control_message,
    .write_uncoded_frame        = bluelet_write_frame,
    .check_bitstream            = bluelet_enc_check_bitstream,
    .p.flags                    = AVFMT_NOFILE | AVFMT_TS_NONSTRICT | AVFMT_NOTIMESTAMPS,
    .p.priv_class               = &bluelet_muxer_class,
};
