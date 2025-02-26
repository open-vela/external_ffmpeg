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
 * Bluelet input and output: common code
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#ifdef CONFIG_NET_RPMSG
#include <netpacket/rpmsg.h>
#endif // CONFIG_NET_RPMSG
#ifdef CONFIG_UORB
#include <connectivity/bt.h>
#include <uORB/uORB.h>
#endif

#include "bluelet.h"
#include "libavutil/time.h"
#include "libavutil/mem.h"
#include "libavcodec/avcodec.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

enum {
    BLUELET_CODEC_TYPE_SBC,
    BLUELET_CODEC_TYPE_MPEG1_2_AUDIO,
    BLUELET_CODEC_TYPE_MPEG2_4_AAC,
    BLUELET_CODEC_TYPE_ATRAC,
    BLUELET_CODEC_TYPE_OPUS,
    BLUELET_CODEC_TYPE_H263,
    BLUELET_CODEC_TYPE_MPEG4_VSP,
    BLUELET_CODEC_TYPE_H263_PROF3,
    BLUELET_CODEC_TYPE_H263_PROF8,
    BLUELET_CODEC_TYPE_LHDC,
    BLUELET_CODEC_TYPE_NON_A2DP,
    BLUELET_CODEC_TYPE_LC3,
};

enum {
    BLUELET_CODEC_BITS_PER_SAMPLE_8 =  0x0,
    BLUELET_CODEC_BITS_PER_SAMPLE_16 = 0x1,
};

enum {
    BLUELET_CODEC_CHANNEL_MODE_MONO = 0x0,
    BLUELET_CODEC_CHANNEL_MODE_STEREO = 0x1
};

enum {
    BLUELET_CTRL_CMD_START,
    BLUELET_CTRL_CMD_STOP,
    BLUELET_CTRL_CMD_CONFIG_DONE
};

enum {
    BLUELET_CTRL_EVT_STARTED,
    BLUELET_CTRL_EVT_START_FAIL,
    BLUELET_CTRL_EVT_STOPPED,
    BLUELET_CTRL_EVT_UPDATE_CONFIG,
};

enum {
    BLUELET_A2DP_IPC,
    BLUELET_LEA_IPC,
};

#define BLUELET_AAC_OBJECT_TYPE_MPEG2_LC 0x80  /* MPEG-2 Low Complexity */
#define BLUELET_AAC_OBJECT_TYPE_MPEG4_LC 0x40  /* MPEG-4 Low Complexity */
#define BLUELET_AAC_OBJECT_TYPE_MPEG4_LTP 0x20 /* MPEG-4 Long Term Prediction */
#define BLUELET_AAC_OBJECT_TYPE_MPEG4_SCALABLE 0x10

typedef struct {
    uint32_t codec_type;
    uint32_t sample_rate;
    uint32_t bit_per_sample;
    uint32_t channels;
    uint32_t bit_rate;
    uint32_t frame_size;
    uint32_t packet_size;
} bluelet_config_t;

typedef struct {
    uint32_t channel_mode;
    uint32_t blocks;
    uint32_t subbands;
    uint32_t alloc_method;
    uint32_t bitpool;
} bluelet_sbc_param_t;

typedef struct {
    uint32_t object_type;
    uint32_t vbr;
} bluelet_aac_param_t;

struct bluelet_ipc_pair {
    const char *ctrl;
    const char *data;
};

static const struct bluelet_ipc_pair ipc_pair[2][2] = {
    {
        { "a2dp_sink_ctrl", "a2dp_sink_data" },
        { "a2dp_source_ctrl", "a2dp_source_data" },
    },
    {
        { "lea_sink_ctrl", "lea_sink_data" },
        { "lea_source_ctrl", "lea_source_data" },
    },
};

/*****************************************************************************
 *  Functions
 *****************************************************************************/

static void ff_bluelet_socket_disconnect(int socket_fd)
{
    if (socket_fd > 0) {
        close(socket_fd);
    }
}

static int ff_bluelet_socket_connect(const char *server_name, const char *path, bool nonblock)
{
    int socket_fd;
    int ret;
    int flags = SOCK_STREAM | SOCK_CLOEXEC;

    if (nonblock)
        flags |= SOCK_NONBLOCK;

    if (strcmp(server_name, "local") == 0) {
        struct sockaddr_un addr;
        socket_fd = socket(AF_LOCAL, flags, 0);
        if (socket_fd < 0)
            return socket_fd;

        memset(&addr, 0, sizeof(addr));
        strcpy(addr.sun_path, path);
        addr.sun_family = AF_UNIX;

        ret = connect(socket_fd, (struct sockaddr*)&addr, sizeof(addr));
    } else {
#ifdef CONFIG_NET_RPMSG
        struct sockaddr_rpmsg addr;
        socket_fd = socket(AF_RPMSG, flags, 0);
        if (socket_fd < 0)
            return socket_fd;
        memset(&addr, 0, sizeof(addr));
        strcpy(addr.rp_name, path);
        strcpy(addr.rp_cpu, server_name);
        addr.rp_family = AF_RPMSG;
        ret = connect(socket_fd, (struct sockaddr*)&addr, sizeof(addr));
#else
        return -1;
#endif
    }

    if (ret < 0 && errno != EINPROGRESS) {
        close(socket_fd);
        return ret;
    }

    return socket_fd;
}

static int ff_bluelet_recv_ctrl(BlueletPriv *priv, void *buffer, size_t length)
{
    assert(priv->ctrl_fd > 0);

    while(length > 0) {
        ssize_t ret = recv(priv->ctrl_fd, buffer, length, MSG_NOSIGNAL);
        if (ret < 0)
            return AVERROR(errno);

        if (ret == 0)
            return AVERROR(ENOTCONN);

        buffer = (char*)buffer + ret;
        length -= ret;
    }

    return 0;
}

static int ff_bluelet_send_ctrl(BlueletPriv *priv, const void *buffer, size_t length)
{
    assert(priv->ctrl_fd > 0);

    while (length > 0) {
        ssize_t ret = send(priv->ctrl_fd, buffer, length, MSG_NOSIGNAL);
        if (ret < 0)
            return AVERROR(errno);

        buffer = (const char*)buffer + ret;
        length -= ret;
    }

    return 0;
}

static int ff_bluelet_connect(BlueletPriv *priv, bool nonblock)
{
    uint8_t type;

    if (!strcmp(priv->mode, "a2dp")) {
        type = BLUELET_A2DP_IPC;
    } else if (!strcmp(priv->mode, "lea")) {
        type = BLUELET_LEA_IPC;
    } else {
        return AVERROR_INVALIDDATA;
    }

    if (priv->ctrl_fd <= 0) {
        priv->ctrl_fd = ff_bluelet_socket_connect(priv->server_name, ipc_pair[type][priv->playback].ctrl, nonblock);
        if (priv->ctrl_fd < 0)
            return AVERROR(errno);
    }

    if (priv->data_fd <= 0) {
        priv->data_fd = ff_bluelet_socket_connect(priv->server_name, ipc_pair[type][priv->playback].data, nonblock);
        if (priv->data_fd < 0)
            return AVERROR(errno);
    }
    priv->state = BLUELET_STATE_IDLE;

    return 0;
}

int ff_bluelet_disconnect(BlueletPriv *priv)
{
    ff_bluelet_socket_disconnect(priv->ctrl_fd);
    ff_bluelet_socket_disconnect(priv->data_fd);

    priv->ctrl_fd = 0;
    priv->data_fd = 0;
    priv->ctrl_connected = false;
    priv->data_connected = false;
    priv->state = BLUELET_STATE_IDLE;

    return 0;
}

int ff_bluelet_init(BlueletPriv *priv, bool nonblock)
{
    int ret;

    priv->nonblock = nonblock;

#ifdef CONFIG_UORB
    priv->uorb_fd = orb_subscribe(ORB_ID(bt_stack_state));
    if (priv->uorb_fd < 0)
        return priv->uorb_fd;
#else
    /* if can't receive orb message, try connect a2dp server */

    ret = ff_bluelet_connect(priv, nonblock);
    if (ret < 0)
        return ret;
#endif
    priv->state = BLUELET_STATE_IDLE;

    return 0;
}

void ff_bluelet_deinit(BlueletPriv *priv)
{
#ifdef CONFIG_UORB
    if (priv->uorb_fd > 0)
        orb_unsubscribe(priv->uorb_fd);
#endif
    ff_bluelet_disconnect(priv);
}

int ff_bluelet_read_buffer(BlueletPriv *priv, void *buffer, size_t bytes)
{
    int ret;

    if (priv->state == BLUELET_STATE_STARTING)
        return AVERROR(EAGAIN);

    if (priv->state != BLUELET_STATE_STARTED)
        return AVERROR_EOF;

    ret = recv(priv->data_fd, buffer, bytes, MSG_NOSIGNAL);
    if (ret < 0)
        return AVERROR(errno);
    else if (ret == 0)
        return AVERROR_EOF;

    return ret;
}

int ff_bluelet_write_buffer(BlueletPriv *priv, void *buffer, size_t bytes)
{
    int ret;

    if (priv->state == BLUELET_STATE_STARTING)
        return AVERROR(EAGAIN);

    if (priv->state != BLUELET_STATE_STARTED)
        return AVERROR_EOF;

    ret = send(priv->data_fd, buffer, bytes, MSG_NOSIGNAL);
    if (ret < 0) {
        if (errno == EAGAIN)
            return 0;
        else
            return AVERROR(errno);
    }

    return ret;
}

static int ff_bluelet_update_config(BlueletPriv *priv)
{
    int ret;
    uint8_t is_valid;
    bluelet_config_t config;
    uint8_t cmd = BLUELET_CTRL_CMD_CONFIG_DONE;

    ret = ff_bluelet_recv_ctrl(priv, &is_valid, 1);
    if (ret < 0 || is_valid == 0)
        goto error;

    ret = ff_bluelet_recv_ctrl(priv, &config, sizeof(config));
    if (ret < 0)
        goto error;

    // Check the codec type
    if (config.codec_type == BLUELET_CODEC_TYPE_SBC)
        priv->codec_id = priv->playback ? AV_CODEC_ID_SBC : AV_CODEC_ID_SBC_PACKED_A2DP;
    else if (config.codec_type == BLUELET_CODEC_TYPE_MPEG2_4_AAC)
        priv->codec_id = priv->playback ? AV_CODEC_ID_AAC : AV_CODEC_ID_AAC_LATM_A2DP;
    else if (config.codec_type == BLUELET_CODEC_TYPE_LC3)
        priv->codec_id = AV_CODEC_ID_LC3;
    else
        goto error;

    priv->sample_rate = config.sample_rate;
    // Check the codec config bits per sample
    switch (config.bit_per_sample) {
    case BLUELET_CODEC_BITS_PER_SAMPLE_8:
        priv->sample_fmt = AV_SAMPLE_FMT_U8;
        break;
    case BLUELET_CODEC_BITS_PER_SAMPLE_16:
        priv->sample_fmt = AV_SAMPLE_FMT_S16;
        break;
    default:
        goto error;
    }

    // Check the codec config channel mode
    switch (config.channels) {
    case BLUELET_CODEC_CHANNEL_MODE_MONO: {
        priv->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        priv->channels = 1;
        break;
    }
    case BLUELET_CODEC_CHANNEL_MODE_STEREO: {
        priv->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        priv->channels = 2;
        break;
    }
    default:
        goto error;
    }

    priv->bit_rate = config.bit_rate;
    priv->frame_size = config.frame_size;
    priv->packet_size = config.packet_size;
    if (config.codec_type == BLUELET_CODEC_TYPE_SBC) {
        bluelet_sbc_param_t param;

        ret = ff_bluelet_recv_ctrl(priv, &param, sizeof(param));
        if (ret < 0)
            goto error;

        snprintf(priv->sbc.param, sizeof(priv->sbc.param),
                "channel_mode=%" PRIu32 ":blocks=%" PRIu32 ":subbands=%" PRIu32 ":alloc_method=%" PRIu32 ":bitpool=%" PRIu32,
                param.channel_mode, param.blocks, param.subbands, param.alloc_method, param.bitpool);
    } else if (config.codec_type == BLUELET_CODEC_TYPE_MPEG2_4_AAC) {
        bluelet_aac_param_t param;
        ret = ff_bluelet_recv_ctrl(priv, &param, sizeof(param));
        if (ret < 0)
            goto error;

        priv->aac.vbr = param.vbr;
        switch (param.object_type) {
            case BLUELET_AAC_OBJECT_TYPE_MPEG2_LC:
                priv->aac.profile = FF_PROFILE_MPEG2_AAC_LOW;
                break;
            case BLUELET_AAC_OBJECT_TYPE_MPEG4_LC:
                priv->aac.profile = FF_PROFILE_AAC_LOW;
                break;
            case BLUELET_AAC_OBJECT_TYPE_MPEG4_LTP:
                priv->aac.profile = FF_PROFILE_AAC_LTP;
                break;
            case BLUELET_AAC_OBJECT_TYPE_MPEG4_SCALABLE:
                priv->aac.profile = FF_PROFILE_AAC_SSR;
                break;
            default:
                break;
        }
    }

    priv->state = BLUELET_STATE_CONFIGED;
    ff_bluelet_send_ctrl(priv, &cmd, 1);

    return BLUELET_ACTION_CONFIG;

error:
    priv->codec_id = AV_CODEC_ID_NONE;
    priv->state = BLUELET_STATE_IDLE;
    return BLUELET_ACTION_AVAILABLE;
}

int ff_bluelet_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                      const AVCodec* codec, const char *key, int flags)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    BlueletPriv *priv = devcap->device_context->priv_data;
    struct AVOptionRanges *ranges;
    int i, nb = 0, ret = AVERROR(ENOMEM);

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        goto err;

    ranges->nb_components = 1;

    if (!strcmp(key, "sample_fmts")) {
        while (codec->sample_fmts[nb] != AV_SAMPLE_FMT_NONE) nb++;

        ranges->range = av_mallocz(sizeof(AVOptionRange*) * nb);
        if (!ranges->range)
            goto err;
        ranges->nb_ranges = nb;

        for (i = 0; i < nb; i++) {
            ranges->range[i] = av_mallocz(sizeof(AVOptionRange));
            if (!ranges->range[i])
                goto err;
            ranges->range[i]->value_min = codec->sample_fmts[i];
            ranges->range[i]->value_max = ranges->range[i]->value_min;
        }

    } else if (!strcmp(key, "channels")) {
        ranges->range = av_mallocz(sizeof(AVOptionRange*));
        if (!ranges->range)
            goto err;
        ranges->nb_ranges = 1;
    
        ranges->range[0] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[0])
            goto err;
        ranges->range[0]->value_min = priv->channels;
        ranges->range[0]->value_max = priv->channels;

    } else if (!strcmp(key, "sample_rates")) {
        ranges->range = av_mallocz(sizeof(AVOptionRange*));
        if (!ranges->range)
            goto err;
        ranges->nb_ranges = 1;
    
        ranges->range[0] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[0])
            goto err;
        ranges->range[0]->value_min = priv->sample_rate;
        ranges->range[0]->value_max = priv->sample_rate;

    } else if (!strcmp(key, "codecs")) {
        while (codec->sample_fmts[nb] != AV_SAMPLE_FMT_NONE) nb++;

        ranges->range = av_mallocz(sizeof(AVOptionRange*) * nb);
        if (!ranges->range)
            goto err;
        ranges->nb_ranges = nb;

        for (i = 0; i < nb; i++) {
            ranges->range[i] = av_mallocz(sizeof(AVOptionRange));
            if (!ranges->range[i])
                goto err;
            ranges->range[i]->value_min = av_get_pcm_codec(codec->sample_fmts[i], -1);
            ranges->range[i]->value_max = ranges->range[i]->value_min;
        }

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

#ifdef CONFIG_UORB
int ff_bluelet_handle_uorb_event(BlueletPriv *priv)
{
    int ret;
    struct bt_stack_state state;

    ret = read(priv->uorb_fd, &state, sizeof(struct bt_stack_state));
    if (ret < 0)
        return ret;

    if (state.state == BT_STACK_STATE_ON)
        ret = ff_bluelet_connect(priv, priv->nonblock);
    else
        ret = ff_bluelet_disconnect(priv);

    return ret < 0 ? ret : 0;
}
#endif

int ff_bluelet_handle_event(BlueletPriv *priv)
{
    int ret;
    uint8_t event;
    int action = BLUELET_ACTION_NONE;

    ret = ff_bluelet_recv_ctrl(priv, &event, 1);
    if (ret < 0)
        return action;

    switch (event) {
    case BLUELET_CTRL_EVT_STARTED:
        action = BLUELET_ACTION_AVAILABLE;
        priv->state = BLUELET_STATE_STARTED;
        break;
    case BLUELET_CTRL_EVT_START_FAIL:
        action = BLUELET_ACTION_AVAILABLE;
        priv->state = BLUELET_STATE_CONFIGED;
        break;
    case BLUELET_CTRL_EVT_STOPPED:
        /* The A2DP codec configurations remain unchanged when audio stream is suspended */
        if (strcmp(priv->mode, "lea") == 0) {
            action = BLUELET_ACTION_CONFIG;
            priv->state = BLUELET_STATE_IDLE;
        }
        break;
    case BLUELET_CTRL_EVT_UPDATE_CONFIG:
        action = ff_bluelet_update_config(priv);
        break;
    default:
        break;
    }

    return action;
}

int ff_bluelet_start(BlueletPriv *priv)
{
    int ret = 0;
    uint8_t cmd = BLUELET_CTRL_CMD_START;

    if (priv->state == BLUELET_STATE_CONFIGED) {
        ret = ff_bluelet_send_ctrl(priv, &cmd, 1);
        if (ret == 0)
            priv->state = BLUELET_STATE_STARTING;
    } else if (priv->state == BLUELET_STATE_IDLE)
        ret = AVERROR(EPERM);
    else if (priv->state == BLUELET_STATE_STARTING)
        ret = AVERROR(EAGAIN);

    return ret;
}

int ff_bluelet_stop(BlueletPriv *priv)
{
    uint8_t cmd = BLUELET_CTRL_CMD_STOP;

    if (priv->state == BLUELET_STATE_STARTING || priv->state == BLUELET_STATE_STARTED) {
        priv->state = BLUELET_STATE_CONFIGED;
        return ff_bluelet_send_ctrl(priv, &cmd, 1);
    }

    return 0;
}
