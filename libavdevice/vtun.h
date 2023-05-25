/*
 * Copyright (c) 2023 xiaomi corp
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

#ifndef AVDEVICE_VTUN_H
#define AVDEVICE_VTUN_H

typedef enum {
    VTUN_FRAME_FORMAT_BGRA8888,
    VTUN_FRAME_FORMAT_YUV420SP,
    VTUN_FRAME_FORMAT_INVALID
} AVVtunFrameFormat;

typedef enum {
    VTUN_CTRL_EVT_NONE,
    VTUN_CTRL_EVT_FRAME_REQ,
    VTUN_CTRL_EVT_PLAY,
    VTUN_CTRL_EVT_PAUSE,
} AVVtunCtrlEvtType;

typedef struct {
    AVVtunFrameFormat format;
    unsigned current_ms;
    size_t size;
    void *addr;
    int w;
    int h;
} AVVtunFrame;

#endif
