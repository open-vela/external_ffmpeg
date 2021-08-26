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
 * FLUORIDE input and output: definitions and structures
 */

#ifndef AVDEVICE_FLUORIDE_H
#define AVDEVICE_FLUORIDE_H

#include "avdevice.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FLUORIDE_AUDIO_DISCONNECTED      (-1)

typedef enum {
    FLUORIDE_STATE_STOPPED,
    FLUORIDE_STATE_STARTING,
    FLUORIDE_STATE_STARTED,
} fluoride_state_t;

typedef struct FluPriv {
    AVClass          *class;
    AVStream         *st;

    int              period_bytes; ///< preferred size for reads and writes, in bytes
    int              period_time;  ///< preferred time for reads and writes, in ms
    int              frame_size;   ///< bytes per sample * channels
    uint32_t         sample_rate;
    uint8_t          channels;
    uint8_t          bit_per_sample;
    uint32_t         bit_rate;
    bool             nonblock;
    bool             playback;
    bool             app_start;
    int              ctrl_fd;
    int              data_fd;
    enum AVCodecID   codec_id;
    fluoride_state_t state;
    AVPacket         *lastpkt;

    int              available;
} FluPriv;

typedef enum {
    FLUORIDE_CTRL_CMD_NONE,
    FLUORIDE_CTRL_CMD_CHECK_READY,
    FLUORIDE_CTRL_CMD_START,
    FLUORIDE_CTRL_CMD_STOP,
    FLUORIDE_CTRL_CMD_SUSPEND,
    FLUORIDE_CTRL_GET_INPUT_AUDIO_CONFIG,
    FLUORIDE_CTRL_SET_INPUT_AUDIO_CONFIG,
    FLUORIDE_CTRL_GET_OUTPUT_AUDIO_CONFIG,
    FLUORIDE_CTRL_SET_OUTPUT_AUDIO_CONFIG,
    FLUORIDE_CTRL_CMD_OFFLOAD_START,
    FLUORIDE_CTRL_GET_PRESENTATION_POSITION,
    FLUORIDE_CTRL_CMD_AUDIO_START,
    FLUORIDE_CTRL_CMD_AUDIO_SUSPEND,
    FLUORIDE_CTRL_CMD_AUDIO_NEXT,
    FLUORIDE_CTRL_CMD_AUDIO_PREV,
} FLUORIDE_CTRL_CMD;

typedef uint8_t FLUORIDE_CODEC_TYPE;

/*****************************************************************************
 *  Functions
 *****************************************************************************/

int ff_fluoride_open(AVFormatContext *ctx);
void ff_fluoride_close(AVFormatContext *ctx);
int ff_fluoride_init_path(FluPriv *priv);
void ff_fluoride_deinit_path(FluPriv *priv);
void ff_fluoride_socket_disconnect(int socket_fd);
int ff_fluoride_ctrl_arrived(FluPriv *priv, FLUORIDE_CTRL_CMD command);
int ff_fluoride_data_arrived(FluPriv *priv, int path, FLUORIDE_CTRL_CMD *command);
int ff_fluoride_read_input_config(FluPriv *priv);
int ff_fluoride_read_output_config(FluPriv *priv);
ssize_t ff_fluoride_read_buffer(FluPriv *priv, void* buffer, size_t bytes);
ssize_t ff_fluoride_write_buffer(FluPriv *priv, void* buffer, size_t bytes);
int ff_fluoride_resp_arrived(FluPriv *priv);

#endif /* AVDEVICE_FLUORIDE_H */
