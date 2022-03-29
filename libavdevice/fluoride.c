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
 * FLUORIDE input and output: common code
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "libavutil/time.h"
#include "fluoride.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FLUORIDE_MEDIA_CT_SBC 0x00 /* SBC media codec type */
#define FLUORIDE_MEDIA_CT_AAC 0x02 /* AAC media codec type */

typedef enum {
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_NONE   = 0x0,
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_44100  = 0x1 << 0,
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_48000  = 0x1 << 1,
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_88200  = 0x1 << 2,
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_96000  = 0x1 << 3,
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_176400 = 0x1 << 4,
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_192000 = 0x1 << 5,
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_16000  = 0x1 << 6,
    FLUORIDE_BTAV_CODEC_SAMPLE_RATE_24000  = 0x1 << 7
} fluoride_btav_codec_sample_rate_t;

typedef enum {
    FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_NONE = 0x0,
    FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_16   = 0x1 << 0,
    FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_24   = 0x1 << 1,
    FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_32   = 0x1 << 2
} fluoride_btav_codec_bits_per_sample_t;

typedef enum {
    FLUORIDE_BTAV_CODEC_CHANNEL_MODE_NONE   = 0x0,
    FLUORIDE_BTAV_CODEC_CHANNEL_MODE_MONO   = 0x1 << 0,
    FLUORIDE_BTAV_CODEC_CHANNEL_MODE_STEREO = 0x1 << 1
} fluoride_btav_codec_channel_mode_t;

/*
 * Enum values for each FLUORIDE supported codec.
 * There should be a separate entry for each FLUORIDE codec that is supported
 * for encoding (SRC), and for decoding purpose (SINK).
 */
typedef enum {
    FLUORIDE_BTAV_CODEC_INDEX_SOURCE_MIN = 0,
    FLUORIDE_BTAV_CODEC_INDEX_SOURCE_SBC = 0,
    FLUORIDE_BTAV_CODEC_INDEX_SOURCE_AAC,
    FLUORIDE_BTAV_CODEC_INDEX_SOURCE_APTX,
    FLUORIDE_BTAV_CODEC_INDEX_SOURCE_APTX_HD,
    FLUORIDE_BTAV_CODEC_INDEX_SOURCE_LDAC,

    FLUORIDE_BTAV_CODEC_INDEX_SOURCE_MAX,

    FLUORIDE_BTAV_CODEC_INDEX_SINK_MIN = FLUORIDE_BTAV_CODEC_INDEX_SOURCE_MAX,

    FLUORIDE_BTAV_CODEC_INDEX_SINK_SBC = FLUORIDE_BTAV_CODEC_INDEX_SINK_MIN,
    FLUORIDE_BTAV_CODEC_INDEX_SINK_AAC,
    FLUORIDE_BTAV_CODEC_INDEX_SINK_LDAC,

    FLUORIDE_BTAV_CODEC_INDEX_SINK_MAX,

    FLUORIDE_BTAV_CODEC_INDEX_MIN = FLUORIDE_BTAV_CODEC_INDEX_SOURCE_MIN,
    FLUORIDE_BTAV_CODEC_INDEX_MAX = FLUORIDE_BTAV_CODEC_INDEX_SINK_MAX
} fluoride_btav_codec_index_t;

typedef enum {
    FLUORIDE_CTRL_ACK_SUCCESS,
    FLUORIDE_CTRL_ACK_FAILURE,
} FLUORIDE_CTRL_ACK;

struct fluoride_btav_uipc_pair {
    const char *ctrl;
    const char *data;
};

static struct fluoride_btav_uipc_pair upair[2] =
{
    {"/data/misc/bluedroid/.sink_ctrl",   "/data/misc/bluedroid/.sink_data"  },
    {"/data/misc/bluedroid/.source_ctrl", "/data/misc/bluedroid/.source_data"},
};

/*****************************************************************************
 *  Functions
 *****************************************************************************/
static int ff_fluoride_open_path(FluPriv *priv, const char *path);

void ff_fluoride_socket_disconnect(int socket_fd)
{
    if (socket_fd > 0) {
        close(socket_fd);
    }
}

static int ff_fluoride_socket_connect(const char *path, bool nonblock)
{
    struct sockaddr_un addr;
    int socket_fd;
    int ret;
    int flags = SOCK_STREAM | SOCK_CLOEXEC;

    if (nonblock)
        flags |= SOCK_NONBLOCK;

    socket_fd = socket(AF_LOCAL, flags, 0);
    if (socket_fd < 0)
        return socket_fd;

    memset(&addr, 0, sizeof(addr));
    strcpy(addr.sun_path, path);
    addr.sun_family = AF_UNIX;

    ret = connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1 && errno != EINPROGRESS) {
      close(socket_fd);
      return ret;
    }

    return socket_fd;
}

static ssize_t ff_fluoride_receive_ctrl(FluPriv *priv, void* buffer, size_t length)
{
    assert(priv->ctrl_fd > 0);
    return recv(priv->ctrl_fd, buffer, length, MSG_NOSIGNAL);
}

static int ff_fluoride_send_ctrl(FluPriv *priv, const void* buffer, size_t length)
{
    size_t  remaining = length;
    ssize_t sent;

    assert(priv->ctrl_fd > 0);
    while (remaining > 0)
    {
        sent = send(priv->ctrl_fd, buffer, remaining, MSG_NOSIGNAL);
        if (sent == (ssize_t)remaining) {
            remaining = 0;
            break;
        }

        if (sent > 0) {
            buffer = (char*)buffer + sent;
            remaining -= sent;
            continue;
        }

        if (sent < 0)
            break;
    }

    if (remaining > 0)
        return AVERROR(errno);

    return length;
}

static int ff_fluoride_send_command(FluPriv *priv, FLUORIDE_CTRL_CMD cmd)
{
    char ack = FLUORIDE_CTRL_ACK_FAILURE;
    int  ret;

    if (priv->ctrl_fd == FLUORIDE_AUDIO_DISCONNECTED){
        ret = ff_fluoride_open_path(priv, upair[priv->playback].ctrl);
        if (ret < 0)
            return AVERROR(EAGAIN);
    }

    ret = send(priv->ctrl_fd, &cmd, 1, MSG_NOSIGNAL);
    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &ack, 1);

    if (ack != FLUORIDE_CTRL_ACK_SUCCESS)
        ret = AVERROR(EAGAIN);

    return ret;
}

static int ff_fluoride_receive_command(FluPriv *priv, FLUORIDE_CTRL_CMD *command)
{
    FLUORIDE_CTRL_CMD cmd;
    int ret;

    ret = ff_fluoride_receive_ctrl(priv, &cmd, sizeof(cmd));
    if (ret > 0) {
        switch (cmd) {
            case FLUORIDE_CTRL_CMD_AUDIO_START:
                *command = FLUORIDE_CTRL_CMD_AUDIO_START;
                break;

            case FLUORIDE_CTRL_CMD_AUDIO_SUSPEND:
                *command = FLUORIDE_CTRL_CMD_AUDIO_SUSPEND;
                ff_fluoride_send_command(priv, FLUORIDE_CTRL_CMD_STOP);
                priv->app_start = false;
                break;

            default:
                break;
        }
    }

    return ret;
}

static int ff_fluoride_open_path(FluPriv *priv, const char *path)
{
    int ret;

    ret = ff_fluoride_socket_connect(path, priv->nonblock);
    if (ret > 0) {
        if (!strcmp(path, upair[priv->playback].ctrl)) {
            priv->ctrl_fd = ret;
            ret = ff_fluoride_send_command(priv, FLUORIDE_CTRL_CMD_CHECK_READY);
        } else if (!strcmp(path, upair[priv->playback].data)) {
            priv->data_fd = ret;
        }
    }

    return ret;
}

int ff_fluoride_init_path(FluPriv *priv)
{
    if ((ff_fluoride_open_path(priv, upair[priv->playback].ctrl)) < 0 ||
        (ff_fluoride_open_path(priv, upair[priv->playback].data)) < 0)
        return -1;

    return 0;
}

void ff_fluoride_deinit_path(FluPriv *priv)
{
    ff_fluoride_socket_disconnect(priv->ctrl_fd);
    priv->ctrl_fd = FLUORIDE_AUDIO_DISCONNECTED;

    ff_fluoride_socket_disconnect(priv->data_fd);
    priv->data_fd = FLUORIDE_AUDIO_DISCONNECTED;
}

static int ff_fluoride_socket_read(int socket_fd, void* p, size_t len, bool nonblock)
{
    assert(socket_fd > 0);
    return recv(socket_fd, p, len, MSG_NOSIGNAL);
}

static int ff_fluoride_socket_write(int socket_fd, const void* p, size_t len, bool nonblock)
{
    assert(socket_fd > 0);
    return send(socket_fd, p, len, MSG_NOSIGNAL);
}

int ff_fluoride_read_buffer(FluPriv *priv, void* buffer, size_t bytes)
{
    int ret;

    if (priv->data_fd == FLUORIDE_AUDIO_DISCONNECTED) {
        return AVERROR_EOF;
    }

    ret = ff_fluoride_socket_read(priv->data_fd, buffer, bytes, priv->nonblock);
    if (ret <= 0) {
        if (errno == EAGAIN)
            ret = AVERROR(errno);
        else {
            ff_fluoride_socket_disconnect(priv->data_fd);
            priv->data_fd = FLUORIDE_AUDIO_DISCONNECTED;
            ret = AVERROR_EOF;
        }
    }

    return ret;
}

int ff_fluoride_write_buffer(FluPriv *priv, void* buffer, size_t bytes)
{
    int ret;

    if (priv->ctrl_fd == FLUORIDE_AUDIO_DISCONNECTED)
        return AVERROR_EOF;

    if (priv->data_fd == FLUORIDE_AUDIO_DISCONNECTED) {
        ret = ff_fluoride_open_path(priv, upair[priv->playback].data);
        if (ret < 0)
            return AVERROR(EAGAIN);
    }

    ret = ff_fluoride_socket_write(priv->data_fd, buffer, bytes, priv->nonblock);
    if (ret <= 0) {
        if (errno == EAGAIN)
            ret = AVERROR(errno);
        else {
            ff_fluoride_socket_disconnect(priv->data_fd);
            priv->data_fd = FLUORIDE_AUDIO_DISCONNECTED;
            priv->state = FLUORIDE_STATE_STOPPED;
            ret = AVERROR_EOF;
        }
    }

    return ret;
}

static int ff_fluoride_write_audio_config(FluPriv *priv, FLUORIDE_CTRL_CMD cmd)
{
    fluoride_btav_codec_sample_rate_t sample_rate = FLUORIDE_BTAV_CODEC_SAMPLE_RATE_44100;
    fluoride_btav_codec_channel_mode_t channel_mode = FLUORIDE_BTAV_CODEC_CHANNEL_MODE_STEREO;
    fluoride_btav_codec_bits_per_sample_t bits_per_sample = FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_16;
    int      ret;

    ret = ff_fluoride_send_command(priv, cmd);
    if (ret <= 0)
        return ret;

    switch (priv->sample_rate) {
        case 44100:
            sample_rate = FLUORIDE_BTAV_CODEC_SAMPLE_RATE_44100;
            break;
        case 48000:
            sample_rate = FLUORIDE_BTAV_CODEC_SAMPLE_RATE_48000;
            break;
        case 88200:
            sample_rate = FLUORIDE_BTAV_CODEC_SAMPLE_RATE_88200;
            break;
        case 96000:
            sample_rate = FLUORIDE_BTAV_CODEC_SAMPLE_RATE_96000;
            break;
        case 176400:
            sample_rate = FLUORIDE_BTAV_CODEC_SAMPLE_RATE_176400;
            break;
        case 192000:
            sample_rate = FLUORIDE_BTAV_CODEC_SAMPLE_RATE_192000;
            break;
        default:
            sample_rate = FLUORIDE_BTAV_CODEC_SAMPLE_RATE_44100;
            break;
    }

    ret = ff_fluoride_send_ctrl(priv, &sample_rate, sizeof(sample_rate));
    if (ret < 0)
            return -1;

    switch (priv->bit_per_sample) {
        case 16:
            bits_per_sample = FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_16;
            break;
        case 24:
            bits_per_sample = FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_24;
            break;
        case 32:
            bits_per_sample = FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_32;
            break;
        default:
            bits_per_sample = FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_16;
            break;
    }

    ret = ff_fluoride_send_ctrl(priv, &bits_per_sample, sizeof(bits_per_sample));
    if (ret < 0)
        return -1;

    if (priv->channels == 1)
        channel_mode = FLUORIDE_BTAV_CODEC_CHANNEL_MODE_MONO;
    return ff_fluoride_send_ctrl(priv, &channel_mode, sizeof(channel_mode));
}

int ff_fluoride_read_input_config(FluPriv *priv)
{
    FLUORIDE_CODEC_TYPE codec_type;
    int                 ret;

    ret = ff_fluoride_send_command(priv, FLUORIDE_CTRL_GET_INPUT_AUDIO_CONFIG);
    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &priv->sample_rate, sizeof(priv->sample_rate));
    if (priv->sample_rate == 0)
        priv->sample_rate = 44100;

    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &priv->channels, sizeof(priv->channels));

    if (priv->channels == 0)
        priv->channels = 2;

    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &codec_type, sizeof(FLUORIDE_CODEC_TYPE));
    if (ret > 0) {
        switch (codec_type) {
        case FLUORIDE_MEDIA_CT_SBC:
            priv->codec_id = AV_NE(AV_CODEC_ID_SBC_PACKED, AV_CODEC_ID_SBC_PACKED);
            break;

        case FLUORIDE_MEDIA_CT_AAC:
            priv->codec_id = AV_NE(AV_CODEC_ID_AAC_LATM, AV_CODEC_ID_AAC_LATM);
            break;

        default:
            priv->codec_id = AV_NE(AV_CODEC_ID_SBC_PACKED, AV_CODEC_ID_SBC_PACKED);
            break;
        }
    }

    return ret;
}

int ff_fluoride_read_output_config(FluPriv *priv)
{
    fluoride_btav_codec_index_t codec_type = FLUORIDE_BTAV_CODEC_INDEX_SINK_MAX;
    fluoride_btav_codec_sample_rate_t sample_rate = 0;
    fluoride_btav_codec_channel_mode_t channel_mode = 0;
    fluoride_btav_codec_bits_per_sample_t bits_per_sample = 0;
    uint32_t bit_rate;
    int ret;

    // Receive the current codec config
    ret = ff_fluoride_send_command(priv, FLUORIDE_CTRL_GET_OUTPUT_AUDIO_CONFIG);
    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &codec_type,
            sizeof(fluoride_btav_codec_index_t));

    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &sample_rate,
            sizeof(fluoride_btav_codec_sample_rate_t));

    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &bits_per_sample,
            sizeof(fluoride_btav_codec_bits_per_sample_t));

    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &channel_mode,
            sizeof(fluoride_btav_codec_channel_mode_t));

    if (ret > 0)
        ret = ff_fluoride_receive_ctrl(priv, &priv->bit_rate,
            sizeof(uint32_t));

    // Check the codec type
    if (codec_type == FLUORIDE_BTAV_CODEC_INDEX_SOURCE_SBC)
        priv->codec_id = AV_NE(AV_CODEC_ID_SBC, AV_CODEC_ID_SBC);
    else
        return -1;

    // Check the codec sample rate
    switch (sample_rate) {
        case FLUORIDE_BTAV_CODEC_SAMPLE_RATE_44100:
            priv->sample_rate = 44100;
            break;
        case FLUORIDE_BTAV_CODEC_SAMPLE_RATE_48000:
            priv->sample_rate = 48000;
            break;
        case FLUORIDE_BTAV_CODEC_SAMPLE_RATE_88200:
            priv->sample_rate = 88200;
            break;
        case FLUORIDE_BTAV_CODEC_SAMPLE_RATE_96000:
            priv->sample_rate = 96000;
            break;
        case FLUORIDE_BTAV_CODEC_SAMPLE_RATE_176400:
            priv->sample_rate = 176400;
            break;
        case FLUORIDE_BTAV_CODEC_SAMPLE_RATE_192000:
            priv->sample_rate = 192000;
            break;
        case FLUORIDE_BTAV_CODEC_SAMPLE_RATE_NONE:
        default:
            return -1;
    }

    // Check the codec config bits per sample
    switch (bits_per_sample) {
        case FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_16:
            priv->bit_per_sample = 16;
            break;
        case FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_24:
            priv->bit_per_sample = 24;
            break;
        case FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_32:
            priv->bit_per_sample = 32;
            break;
        case FLUORIDE_BTAV_CODEC_BITS_PER_SAMPLE_NONE:
        default:
            return -1;
    }

    // Check the codec config channel mode
    switch (channel_mode) {
        case FLUORIDE_BTAV_CODEC_CHANNEL_MODE_MONO:
            priv->channels = 1;
            break;
        case FLUORIDE_BTAV_CODEC_CHANNEL_MODE_STEREO:
            priv->channels = 2;
            break;
        case FLUORIDE_BTAV_CODEC_CHANNEL_MODE_NONE:
        default:
            return -1;
    }

    return ret;
}

int ff_fluoride_data_arrived(FluPriv *priv, int path,FLUORIDE_CTRL_CMD *command)
{
    int ret = 0;

    if (path == priv->ctrl_fd)
        ret = ff_fluoride_receive_command(priv, command);

    return ret;
}

int ff_fluoride_ctrl_arrived(FluPriv *priv, FLUORIDE_CTRL_CMD command)
{
    int ret;

    if (command == FLUORIDE_CTRL_CMD_AUDIO_START) {
        ff_fluoride_send_command(priv, FLUORIDE_CTRL_CMD_START);
        priv->app_start = true;
    }
    else if (command == FLUORIDE_CTRL_CMD_AUDIO_SUSPEND) {
        ff_fluoride_send_command(priv, FLUORIDE_CTRL_CMD_STOP);
        priv->app_start = false;
    }

    return ff_fluoride_send_command(priv, command);
}

int ff_fluoride_resp_arrived(FluPriv *priv)
{
    char ack;
    int ret;

    ret = ff_fluoride_receive_ctrl(priv, &ack, 1);

    if (ack != FLUORIDE_CTRL_ACK_SUCCESS) {
        ret = AVERROR(EAGAIN);
    }

    return ret;
}

int ff_fluoride_open(AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;
    int ret = 0;

    if (!priv->playback)
        ret = ff_fluoride_read_input_config(priv);
    else {
        if (priv->state == FLUORIDE_STATE_STOPPED) {
            ret = ff_fluoride_write_audio_config(priv, FLUORIDE_CTRL_SET_OUTPUT_AUDIO_CONFIG);
            if (ret < 0)
                return AVERROR(EAGAIN);
        }
        if (priv->state == FLUORIDE_STATE_STOPPED ||
            priv->state == FLUORIDE_STATE_STARTING) {
          ret = ff_fluoride_send_command(priv, FLUORIDE_CTRL_CMD_START);
          if (ret > 0) {
            priv->state = FLUORIDE_STATE_STARTED;
          } else {
            priv->state = FLUORIDE_STATE_STARTING;
            return AVERROR(EAGAIN);
          }
        }
    }

    return ret;
}

void ff_fluoride_close(AVFormatContext *ctx)
{
    FluPriv *priv = ctx->priv_data;

    ff_fluoride_send_command(priv, FLUORIDE_CTRL_CMD_STOP);
}
