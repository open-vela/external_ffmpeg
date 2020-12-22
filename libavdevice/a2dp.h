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
 * A2DP input and output: definitions and structures
 */

#ifndef AVDEVICE_A2DP_H
#define AVDEVICE_A2DP_H

#include "avdevice.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define A2DP_AUDIO_DISCONNECTED      (-1)

typedef struct A2dpPriv {
    AVClass       *class;
    AVStream      *st;

    int            period_bytes; ///< preferred size for reads and writes, in bytes
    int            period_time;  ///< preferred time for reads and writes, in ms
    int            frame_size;   ///< bytes per sample * channels
    uint32_t       sample_rate;
    uint8_t        channels;
    bool           nonblock;
    bool           playback;
    bool           app_start;
    int            ctrl_fd;
    int            data_fd;
    enum AVCodecID codec_id;

    int            available;
} A2dpPriv;

typedef enum {
    A2DP_CTRL_CMD_NONE,
    A2DP_CTRL_CMD_CHECK_READY,
    A2DP_CTRL_CMD_START,
    A2DP_CTRL_CMD_STOP,
    A2DP_CTRL_CMD_SUSPEND,
    A2DP_CTRL_GET_INPUT_AUDIO_CONFIG,
    A2DP_CTRL_SET_INPUT_AUDIO_CONFIG,
    A2DP_CTRL_GET_OUTPUT_AUDIO_CONFIG,
    A2DP_CTRL_SET_OUTPUT_AUDIO_CONFIG,
    A2DP_CTRL_CMD_OFFLOAD_START,
    A2DP_CTRL_GET_PRESENTATION_POSITION,
    A2DP_CTRL_CMD_AUDIO_START,
    A2DP_CTRL_CMD_AUDIO_SUSPEND,
    A2DP_CTRL_CMD_AUDIO_NEXT,
    A2DP_CTRL_CMD_AUDIO_PREV,
} tA2DP_CTRL_CMD;

typedef uint32_t tA2DP_SAMPLE_RATE;
typedef uint8_t tA2DP_CHANNEL_COUNT;
typedef uint8_t tA2DP_BITS_PER_SAMPLE;
typedef uint8_t tA2DP_CODEC_TYPE;

/*****************************************************************************
 *  Functions
 *****************************************************************************/

int ff_a2dp_open(AVFormatContext *ctx);
void ff_a2dp_close(AVFormatContext *ctx);
int ff_a2dp_init_path(A2dpPriv *a2dp);
void ff_a2dp_deinit_path(A2dpPriv *a2dp);
void ff_a2dp_socket_disconnect(int socket_fd);
int ff_a2dp_ctrl_arrived(A2dpPriv *a2dp, tA2DP_CTRL_CMD command);
int ff_a2dp_data_arrived(A2dpPriv *a2dp, int path, tA2DP_CTRL_CMD *command);
int ff_a2dp_read_input_config(A2dpPriv *a2dp);
ssize_t ff_a2dp_read_buffer(A2dpPriv *a2dp, void* buffer, size_t bytes);

#endif /* AVDEVICE_A2DP_H */
