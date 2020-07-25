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

#include "libavutil/log.h"
#include "timefilter.h"
#include "avdevice.h"

typedef struct NuttxPriv {
    AVClass     *class;

    struct ap_buffer_s **abuffer;
    int         free_abuffer;

    char        mqname[16];   ///< message queue name
    mqd_t       mq;           ///< message queue
    int         fd;           ///< nuttx device fd

    int         periods;      ///< buffer pereids
    int         period_bytes; ///< preferred size for reads and writes, in bytes

    int         frame_size;   ///< bytes per sample * channels
    uint32_t    sample_rate;
    uint32_t    channels;
    uint32_t    channel_layout;
    bool        playback;
    bool        nonblock;

    bool        mute;
    double      volume;

    uint8_t     *buffer;
    int         buffer_pos;

    int64_t     timestamp;    ///< current timestamp, without latency applied.
    int         last_period;
    TimeFilter *timefilter;
} NuttxPriv;

av_cold int ff_nuttx_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                            const char *key, int flags, bool playback);
av_cold int ff_nuttx_get_device_list(struct AVDeviceInfoList *device_list, bool playback);
av_cold int ff_nuttx_open(NuttxPriv *priv, const char *device, enum AVCodecID);
av_cold int ff_nuttx_close(NuttxPriv *priv);

int ff_nuttx_write_period(NuttxPriv *priv, uint8_t *buf, int size);
int ff_nuttx_read_period(NuttxPriv *priv, uint8_t *buf, int *size);

int ff_nuttx_set_volume(struct AVFormatContext *s1, NuttxPriv *priv, double volume);
int ff_nuttx_set_mute(struct AVFormatContext *s1, NuttxPriv *priv, bool mute);
int ff_nuttx_notify_changed(struct AVFormatContext *s1, NuttxPriv *priv, bool volume);

#endif /* AVDEVICE_NUTTX_H */
