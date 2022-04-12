/*
 * NUTTX input and output
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
 * NUTTX input and output: definitions and structures
 */

#ifndef AVDEVICE_NUTTX_H
#define AVDEVICE_NUTTX_H

#include <nuttx/config.h>
#include <nuttx/audio/audio.h>

#include <mqueue.h>

#include "libavutil/log.h"
#include "avdevice.h"

typedef struct NuttxPriv {
    AVClass    *class;

    bool       running;
    bool       flushing;

    char       devname[32];  ///< device name
    char       mqname[16];   ///< message queue name
    mqd_t      mq;           ///< message queue
    int        fd;           ///< nuttx device fd

    int        periods;      ///< buffer pereids
    int        period_bytes; ///< preferred size for reads and writes, in bytes
    int        period_time;  ///< preferred time for reads and writes, in ms

    int        codec;
    int        frame_size;   ///< bytes per sample * channels
    uint32_t   sample_rate;
    uint32_t   channels;
    uint64_t   channel_layout;
    bool       nonblock;

    bool       pause;
    bool       mute;
    double     volume;

    dq_queue_t bufferq;

    AVPacket   *lastpkt;
    int64_t    captured;
} NuttxPriv;

int ff_nuttx_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                    const char *key, int flags, bool playback);
int ff_nuttx_get_device_list(struct AVDeviceInfoList *device_list, bool playback);

int ff_nuttx_init(NuttxPriv *priv, const char *device);
void ff_nuttx_deinit(NuttxPriv *priv);

int ff_nuttx_open(NuttxPriv *priv, bool playback);
void ff_nuttx_close(NuttxPriv *priv, bool nonblock);

int ff_nuttx_poll_available(NuttxPriv *priv, bool nonblock);
int ff_nuttx_write_data(NuttxPriv *priv, const uint8_t *data, int size);
int ff_nuttx_read_data(NuttxPriv *priv, uint8_t *data, int size);

int ff_nuttx_set_volume(struct AVFormatContext *s1, NuttxPriv *priv, double volume);
int ff_nuttx_set_mute(struct AVFormatContext *s1, NuttxPriv *priv, bool mute);
int ff_nuttx_notify_changed(struct AVFormatContext *s1, NuttxPriv *priv, bool volume);

#endif /* AVDEVICE_NUTTX_H */
