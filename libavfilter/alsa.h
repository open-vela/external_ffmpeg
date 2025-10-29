/*
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
 * alsa common handle
 */

#ifndef AVFILTER_ALSA_H
#define AVFILTER_ALSA_H

#include <libavutil/frame.h>
#include <libavutil/log.h>

#include <alsa/asoundlib.h>
#include <nuttx/audio/audio.h>
#include <nuttx/config.h>

typedef struct AlsaHandle {
    snd_pcm_t *h;
    bool draining;
    int frame_size;
    AVFrame *last_frame;
    int periods;
    int period_time;
    enum AVSampleFormat format;
    int poll_available;
    uint32_t sample_rate;
    AVChannelLayout ch_layout;
} AlsaHandle;

int alsa_open(AlsaHandle *s, const char *device, snd_pcm_stream_t mode,
              int rate, AVChannelLayout ch_layout, enum AVSampleFormat smpfmt,
              int periods, int period_time);
int alsa_close(AlsaHandle *s);
int alsa_write(AlsaHandle *s, void **bufs, int size);
int alsa_read(AlsaHandle *s, void *buffer, int size);
int alsa_query_caps(struct AVOptionRanges **pranges, const char *device,
                    const char *key, bool playback);
int alsa_set_parameter(const char *device, const char *parameter);
#endif /* AVFILTER_ALSA_H */
