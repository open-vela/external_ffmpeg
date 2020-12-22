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
 * A2DP input and output: common code
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
#include "a2dp.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SOCK_SEND_TIMEOUT_MS   2000 /* Timeout for sending */
#define WRITE_POLL_MS          20
#define A2DP_CTRL_PATH         "/data/misc/bluedroid/.a2dp_ctrl"
#define A2DP_DATA_PATH         "/data/misc/bluedroid/.a2dp_data"

#define A2DP_MEDIA_CT_SBC 0x00 /* SBC media codec type */
#define A2DP_MEDIA_CT_AAC 0x02 /* AAC media codec type */

typedef enum {
    BTAV_A2DP_CODEC_SAMPLE_RATE_NONE   = 0x0,
    BTAV_A2DP_CODEC_SAMPLE_RATE_44100  = 0x1 << 0,
    BTAV_A2DP_CODEC_SAMPLE_RATE_48000  = 0x1 << 1,
    BTAV_A2DP_CODEC_SAMPLE_RATE_88200  = 0x1 << 2,
    BTAV_A2DP_CODEC_SAMPLE_RATE_96000  = 0x1 << 3,
    BTAV_A2DP_CODEC_SAMPLE_RATE_176400 = 0x1 << 4,
    BTAV_A2DP_CODEC_SAMPLE_RATE_192000 = 0x1 << 5,
    BTAV_A2DP_CODEC_SAMPLE_RATE_16000  = 0x1 << 6,
    BTAV_A2DP_CODEC_SAMPLE_RATE_24000  = 0x1 << 7
} btav_a2dp_codec_sample_rate_t;

typedef enum {
    BTAV_A2DP_CODEC_BITS_PER_SAMPLE_NONE = 0x0,
    BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16   = 0x1 << 0,
    BTAV_A2DP_CODEC_BITS_PER_SAMPLE_24   = 0x1 << 1,
    BTAV_A2DP_CODEC_BITS_PER_SAMPLE_32   = 0x1 << 2
} btav_a2dp_codec_bits_per_sample_t;

typedef enum {
    BTAV_A2DP_CODEC_CHANNEL_MODE_NONE   = 0x0,
    BTAV_A2DP_CODEC_CHANNEL_MODE_MONO   = 0x1 << 0,
    BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO = 0x1 << 1
} btav_a2dp_codec_channel_mode_t;

/*
 * Enum values for each A2DP supported codec.
 * There should be a separate entry for each A2DP codec that is supported
 * for encoding (SRC), and for decoding purpose (SINK).
 */
typedef enum {
    BTAV_A2DP_CODEC_INDEX_SOURCE_MIN = 0,
    BTAV_A2DP_CODEC_INDEX_SOURCE_SBC = 0,
    BTAV_A2DP_CODEC_INDEX_SOURCE_AAC,
    BTAV_A2DP_CODEC_INDEX_SOURCE_APTX,
    BTAV_A2DP_CODEC_INDEX_SOURCE_APTX_HD,
    BTAV_A2DP_CODEC_INDEX_SOURCE_LDAC,

    BTAV_A2DP_CODEC_INDEX_SOURCE_MAX,

    BTAV_A2DP_CODEC_INDEX_SINK_MIN = BTAV_A2DP_CODEC_INDEX_SOURCE_MAX,

    BTAV_A2DP_CODEC_INDEX_SINK_SBC = BTAV_A2DP_CODEC_INDEX_SINK_MIN,
    BTAV_A2DP_CODEC_INDEX_SINK_AAC,
    BTAV_A2DP_CODEC_INDEX_SINK_LDAC,

    BTAV_A2DP_CODEC_INDEX_SINK_MAX,

    BTAV_A2DP_CODEC_INDEX_MIN = BTAV_A2DP_CODEC_INDEX_SOURCE_MIN,
    BTAV_A2DP_CODEC_INDEX_MAX = BTAV_A2DP_CODEC_INDEX_SINK_MAX
} btav_a2dp_codec_index_t;

typedef enum {
    A2DP_CTRL_ACK_SUCCESS,
    A2DP_CTRL_ACK_FAILURE,
} tA2DP_CTRL_ACK;

/*****************************************************************************
 *  Functions
 *****************************************************************************/

void ff_a2dp_socket_disconnect(int socket_fd)
{
    if (socket_fd > 0) {
        close(socket_fd);
    }
}

static int ff_a2dp_socket_connect(const char *path, bool nonblock)
{
    struct sockaddr_un addr;
    int socket_fd;

    socket_fd = socket(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_fd < 0)
        return socket_fd;

    memset(&addr, 0, sizeof(&addr));
    strcpy(addr.sun_path, path);
    addr.sun_family = AF_UNIX;

    if (connect(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static ssize_t ff_a2dp_receive_ctrl(A2dpPriv *a2dp, void* buffer, size_t length)
{
    int flags = MSG_NOSIGNAL;

    if (a2dp->nonblock)
        flags |= MSG_DONTWAIT;

    assert(a2dp->ctrl_fd > 0);
    return recv(a2dp->ctrl_fd, buffer, length, flags);
}

static int ff_a2dp_send_ctrl(A2dpPriv *a2dp, const void* buffer, size_t length)
{
    size_t  remaining = length;
    ssize_t sent;

    assert(a2dp->ctrl_fd > 0);
    while (remaining > 0)
    {
        sent = send(a2dp->ctrl_fd, buffer, remaining, MSG_NOSIGNAL);
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

static int ff_a2dp_send_command(A2dpPriv *a2dp, tA2DP_CTRL_CMD cmd)
{
    int  ret;
    char ack;
    int  flags = MSG_NOSIGNAL;

    if (a2dp->nonblock)
        flags |= MSG_DONTWAIT;

    assert(a2dp->ctrl_fd > 0);
    ret = send(a2dp->ctrl_fd, &cmd, 1, flags);
    if (ret > 0)
        ret = ff_a2dp_receive_ctrl(a2dp, &ack, 1);

    if (ack != A2DP_CTRL_ACK_SUCCESS) {
        ret = AVERROR(EAGAIN);
    }

    return ret;
}

static int ff_a2dp_receive_command(A2dpPriv *a2dp, tA2DP_CTRL_CMD *command)
{
    tA2DP_CTRL_CMD cmd;
    int ret;

    ret = ff_a2dp_receive_ctrl(a2dp, &cmd, sizeof(cmd));
    if (ret > 0) {
        switch (cmd) {
            case A2DP_CTRL_CMD_AUDIO_START:
                *command = A2DP_CTRL_CMD_AUDIO_START;
                break;

            case A2DP_CTRL_CMD_AUDIO_SUSPEND:
                *command = A2DP_CTRL_CMD_AUDIO_SUSPEND;
                ff_a2dp_send_command(a2dp, A2DP_CTRL_CMD_STOP);
                a2dp->app_start = false;
                break;

            default:
                break;
        }
    }

    return ret;
}

static int ff_a2dp_open_path(A2dpPriv *a2dp, const char *path)
{
    int ret;

    ret = ff_a2dp_socket_connect(path, a2dp->nonblock);
    if (ret > 0) {
        if (!strcmp(path, A2DP_CTRL_PATH)) {
            a2dp->ctrl_fd = ret;
            ret = ff_a2dp_send_command(a2dp, A2DP_CTRL_CMD_CHECK_READY);
        } else if (!strcmp(path, A2DP_DATA_PATH)) {
            a2dp->data_fd = ret;
        }
    }

    return ret;
}

int ff_a2dp_init_path(A2dpPriv *a2dp)
{
    if ((ff_a2dp_open_path(a2dp, A2DP_CTRL_PATH)) < 0 ||
        (ff_a2dp_open_path(a2dp, A2DP_DATA_PATH)) < 0)
        return -1;

    return 0;
}

void ff_a2dp_deinit_path(A2dpPriv *a2dp)
{
    ff_a2dp_socket_disconnect(a2dp->ctrl_fd);
    a2dp->ctrl_fd = A2DP_AUDIO_DISCONNECTED;

    ff_a2dp_socket_disconnect(a2dp->data_fd);
    a2dp->data_fd = A2DP_AUDIO_DISCONNECTED;
}

static int ff_a2dp_socket_read(int socket_fd, void* p, size_t len, bool nonblock)
{
    int flags = MSG_NOSIGNAL;

    if (nonblock)
        flags |= MSG_DONTWAIT;

    assert(socket_fd > 0);
    return recv(socket_fd, p, len, flags);
}

int ff_a2dp_read_buffer(A2dpPriv *a2dp, void* buffer, size_t bytes)
{
    int ret;

    if (a2dp->data_fd == A2DP_AUDIO_DISCONNECTED) {
        return AVERROR_EOF;
    }

    ret = ff_a2dp_socket_read(a2dp->data_fd, buffer, bytes, a2dp->nonblock);
    if (ret <= 0) {
        if (errno == ECONNRESET) {
            ff_a2dp_socket_disconnect(a2dp->data_fd);
            a2dp->data_fd = A2DP_AUDIO_DISCONNECTED;
            ret = AVERROR_EOF;
        }
        else
            ret = AVERROR(errno);
    }

    return ret;
}

static int ff_a2dp_write_audio_config(A2dpPriv *a2dp, tA2DP_CTRL_CMD cmd)
{
    btav_a2dp_codec_sample_rate_t sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
    btav_a2dp_codec_channel_mode_t channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_STEREO;
    btav_a2dp_codec_bits_per_sample_t bits_per_sample = BTAV_A2DP_CODEC_BITS_PER_SAMPLE_16;
    int      ret;

    ret = ff_a2dp_send_command(a2dp, cmd);
    if (ret <= 0)
        return ret;

    switch (a2dp->sample_rate) {
        case 44100:
            sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
            break;
        case 48000:
            sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_48000;
            break;
        case 88200:
            sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_88200;
            break;
        case 96000:
            sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_96000;
            break;
        case 176400:
            sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_176400;
            break;
        case 192000:
            sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_192000;
            break;
        default:
            sample_rate = BTAV_A2DP_CODEC_SAMPLE_RATE_44100;
            break;
    }

    ret = ff_a2dp_send_ctrl(a2dp, &sample_rate, sizeof(sample_rate));
    if (ret < 0)
            return -1;

    if (a2dp->frame_size >= 2 && a2dp->frame_size <= 4) {
        bits_per_sample = 1 << (a2dp->frame_size - 2);
    } else
        return -1;

    ret = ff_a2dp_send_ctrl(a2dp, &bits_per_sample, sizeof(bits_per_sample));
    if (ret < 0)
        return -1;

    if (a2dp->channels == 1)
        channel_mode = BTAV_A2DP_CODEC_CHANNEL_MODE_MONO;
    return ff_a2dp_send_ctrl(a2dp, &channel_mode, sizeof(channel_mode));
}

int ff_a2dp_read_input_config(A2dpPriv *a2dp)
{
    tA2DP_CODEC_TYPE codec_type;
    int              ret;

    ret = ff_a2dp_send_command(a2dp, A2DP_CTRL_GET_INPUT_AUDIO_CONFIG);
    if (ret > 0)
        ret = ff_a2dp_receive_ctrl(a2dp, &a2dp->sample_rate, sizeof(a2dp->sample_rate));
    if (a2dp->sample_rate == 0)
        a2dp->sample_rate = 44100;

    if (ret > 0)
        ret = ff_a2dp_receive_ctrl(a2dp, &a2dp->channels, sizeof(a2dp->channels));

    if (a2dp->channels == 0)
        a2dp->channels = 2;

    if (ret > 0)
        ret = ff_a2dp_receive_ctrl(a2dp, &codec_type, sizeof(tA2DP_CODEC_TYPE));
    if (ret > 0) {
        switch (codec_type) {
        case A2DP_MEDIA_CT_SBC:
            a2dp->codec_id = AV_NE(AV_CODEC_ID_SBC_PACKED, AV_CODEC_ID_SBC_PACKED);
            break;

        case A2DP_MEDIA_CT_AAC:
            a2dp->codec_id = AV_NE(AV_CODEC_ID_AAC_LATM, AV_CODEC_ID_AAC_LATM);
            break;

        default:
            a2dp->codec_id = AV_NE(AV_CODEC_ID_SBC_PACKED, AV_CODEC_ID_SBC_PACKED);
            break;
        }
    }

    return ret;
}

int ff_a2dp_data_arrived(A2dpPriv *a2dp, int path, tA2DP_CTRL_CMD *command)
{
    int ret = 0;

    if (path == a2dp->ctrl_fd)
        ret = ff_a2dp_receive_command(a2dp, command);

    return ret;
}

int ff_a2dp_ctrl_arrived(A2dpPriv *a2dp, tA2DP_CTRL_CMD command)
{
    int ret;

    if (command == A2DP_CTRL_CMD_AUDIO_START) {
        ff_a2dp_send_command(a2dp, A2DP_CTRL_CMD_START);
        a2dp->app_start = true;
    }
    else if (command == A2DP_CTRL_CMD_AUDIO_SUSPEND) {
        ff_a2dp_send_command(a2dp, A2DP_CTRL_CMD_STOP);
        a2dp->app_start = false;
    }

    return ff_a2dp_send_command(a2dp, command);
}

int ff_a2dp_open(AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;
    int ret = 0;

    assert(a2dp->ctrl_fd > 0 && a2dp->data_fd > 0);

    if (!a2dp->playback)
        ret = ff_a2dp_read_input_config(a2dp);
    else
        ret = ff_a2dp_write_audio_config(a2dp, A2DP_CTRL_SET_OUTPUT_AUDIO_CONFIG);

    return ret;
}

void ff_a2dp_close(AVFormatContext *ctx)
{
    A2dpPriv *a2dp = ctx->priv_data;

    ff_a2dp_send_command(a2dp, A2DP_CTRL_CMD_STOP);
}
