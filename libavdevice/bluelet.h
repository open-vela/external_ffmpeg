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
 * Bluelet input and output: definitions and structures
 */

#ifndef AVDEVICE_BLUELET_H
#define AVDEVICE_BLUELET_H

#include "avdevice.h"
#include "avcodec.h"
#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
enum {
    BLUELET_ACTION_NONE,
    BLUELET_ACTION_AVAILABLE,
    BLUELET_ACTION_CONFIG,
};

typedef enum {
    BLUELET_STATE_IDLE,
    BLUELET_STATE_CONFIGED,
    BLUELET_STATE_STARTING,
    BLUELET_STATE_STARTED,
} bluelet_state_t;

typedef struct {
    uint32_t profile;
    uint32_t vbr;
} bluelet_aac_t;

typedef struct {
    uint8_t param[128];
    int nb_out_pkts;
} bluelet_sbc_t;

typedef struct BlueletPriv {
    AVClass*        class;
    AVStream*       st;

    int             frame_size; ///< Number of samples per channel in an audio frame
    uint32_t        sample_rate;
    uint32_t        channels;
    uint32_t        bit_rate;
    uint32_t        sample_fmt;
    uint16_t        packet_size;
    uint32_t        start;
    bool            playback;
    int             ctrl_fd;
    bool            ctrl_connected;
    int             data_fd;
    bool            data_connected;
    enum AVCodecID  codec_id;
    bluelet_state_t state;
    bool            nonblock;
#ifdef CONFIG_UORB
    int             uorb_fd;
#endif
    union {
        AVPacket*   lastpkt;
        bool        available;
    };
    char*           server_name;
    char*           mode;
    union {
        bluelet_aac_t aac;
        bluelet_sbc_t sbc;
    };
    AVChannelLayout ch_layout;

    int64_t recv_ts;
    int64_t send_ts;
} BlueletPriv;

/*****************************************************************************
 *  Functions
 *****************************************************************************/

int ff_bluelet_start(BlueletPriv* priv);
int ff_bluelet_stop(BlueletPriv* priv);
int ff_bluelet_init(BlueletPriv* priv, bool nonblock);
int ff_bluelet_disconnect(BlueletPriv *priv);
void ff_bluelet_deinit(BlueletPriv* priv);
int ff_bluelet_read_buffer(BlueletPriv* priv, void* buffer, size_t bytes);
int ff_bluelet_write_buffer(BlueletPriv* priv, void* buffer, size_t bytes);
#ifdef CONFIG_UORB
int ff_bluelet_handle_uorb_event(BlueletPriv *priv);
#endif
int ff_bluelet_handle_event(BlueletPriv* priv);
int ff_bluelet_capbility_query_ranges(struct AVOptionRanges** ranges_, void* obj,
                                      const AVCodec* codec, const char* key, int flags);

#endif /* AVDEVICE_BLUELET_H */
