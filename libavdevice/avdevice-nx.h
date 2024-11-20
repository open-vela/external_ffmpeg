
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
#ifndef AVDEVICE_AVDEVICE_NX_H
#define AVDEVICE_AVDEVICE_NX_H
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/dict.h"
#include "libavformat/avformat.h"
enum NXAVAppToDevMessageType {
        /**
     * Get format request
     *
     * Get device fromat request info, e.g. BT device need
     * compressed data, request specific bitrate, blocks...
     *
     * data: AVDictionary **: list of options.
     */
    AV_APP_TO_DEV_GET_FORMAT_REQUEST = MKBETAG('G','F','R','Q'),
    AV_APP_TO_DEV_GET_CAPS_REQUEST   = MKBETAG('G','C','P','Q'),
    /**
     * Get fd for poll.
     *
     * Get device fd for polling.
     *
     * data: struct pollfd: terminated by a zeroed element.
     */
    AV_APP_TO_DEV_GET_POLLFD = MKBETAG('G','P','O','L'),
    /**
     * Nofity device poll available.
     *
     * Once poll wakeup by events, then nofity device with this cmd.
     *
     * data: struct pollfd: terminated by a zeroed element.
     */
    AV_APP_TO_DEV_POLL_AVAILABLE = MKBETAG('P','A','V','A'),
    /**
     * Get dump info
     *
     * data: string
     */
    AV_APP_TO_DEV_DUMP = MKBETAG('D','U','M','P'),
    /**
     * Direct set parameter to device.
     *
     * data: string of cmd and arg.
     */
    AV_APP_TO_DEV_SET_PARAMETER = MKBETAG('S', 'E', 'T', 'P'),
    /**
     * Request open/close.
     *
     * Application requests start/stop.
     * trigger adevsrc, devsrc filter one frame to next.
     * or trigger adevsink, devsink set status_in to EOF.
     *
     * data: NULL
     */
    AV_APP_TO_DEV_START = MKBETAG('S', 'T', 'R', 'T'),
    AV_APP_TO_DEV_STOP  = MKBETAG('S', 'T', 'O', 'P'),
    /**
     * Request flush.
     *
     * Application requests flush.
     *
     * data: NULL
     */
    AV_APP_TO_DEV_FLUSH = MKBETAG('F', 'L', 'S', 'H'),
    /**
     * Get position
    */
    AV_APP_TO_DEV_GET_POSITION = MKBETAG('G', 'P', 'O', 'S'),
    /**
     * Request drain.
     *
     * Application requests drain.
    */
    AV_APP_TO_DEV_DRAIN = MKBETAG('D', 'R', 'A', 'N'),
};
enum NXAVDevToAppMessageType {
    AV_DEV_TO_APP_BUFFER_DRAINED = MKBETAG('B', 'D', 'N', ' '),
    AV_DEV_TO_APP_STATE_CHANGED = MKBETAG('C', 'S', 'T', 'A'),
};
/*
 * The packet is an event, which doesn't contain any playable data.
 * Flag indicate this end of the stream.
 */
#define AV_PKT_FLAG_EVT_EOS 0x0020
#endif /* AVDEVICE_AVDEVICE_NX_H */
