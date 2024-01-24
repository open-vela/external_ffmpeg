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

#ifndef AVFILTER_MOVIE_ASYNC_H
#define AVFILTER_MOVIE_ASYNC_H

/**
*  External User Event Callback
*/
#define AVMOVIE_ASYNC_EVENT_NOP                0
#define AVMOVIE_ASYNC_EVENT_PREPARED           1
#define AVMOVIE_ASYNC_EVENT_STARTED            2
#define AVMOVIE_ASYNC_EVENT_PAUSED             3
#define AVMOVIE_ASYNC_EVENT_STOPPED            4
#define AVMOVIE_ASYNC_EVENT_SEEKED             5
#define AVMOVIE_ASYNC_EVENT_COMPLETED          6
#define AVMOVIE_ASYNC_EVENT_CLOSED             7

typedef void (*av_movie_async_event_func)(void* cookie, int event, int ret, const char *extra);

typedef struct AVMovieAsyncEventCookie {
    av_movie_async_event_func event;
    void                      *cookie;
} AVMovieAsyncEventCookie;

/**
*
*  Internal Definitions, src & sink movie async
*/
#define AVMOVIE_ASYNC_STATE_NOP               AVMOVIE_ASYNC_EVENT_NOP
#define AVMOVIE_ASYNC_STATE_PREPARED          AVMOVIE_ASYNC_EVENT_PREPARED
#define AVMOVIE_ASYNC_STATE_STARTED           AVMOVIE_ASYNC_EVENT_STARTED
#define AVMOVIE_ASYNC_STATE_PAUSED            AVMOVIE_ASYNC_EVENT_PAUSED
#define AVMOVIE_ASYNC_STATE_STOPPED           AVMOVIE_ASYNC_EVENT_STOPPED
#define AVMOVIE_ASYNC_STATE_COMPLETED         AVMOVIE_ASYNC_EVENT_COMPLETED

#define AVMOVIE_ASYNC_OPEN                    1
#define AVMOVIE_ASYNC_SET_EVENT               2
#define AVMOVIE_ASYNC_SET_OPTIONS             3
#define AVMOVIE_ASYNC_SET_LOOP                4
#define AVMOVIE_ASYNC_PREPARE                 5
#define AVMOVIE_ASYNC_START                   6
#define AVMOVIE_ASYNC_PAUSE                   7
#define AVMOVIE_ASYNC_SEEK                    8
#define AVMOVIE_ASYNC_PROCESS_COMMAND         9
#define AVMOVIE_ASYNC_COMPLETED               10

#define AVMOVIE_ASYNC_STOP                    100
#define AVMOVIE_ASYNC_RESET                   101
#define AVMOVIE_ASYNC_CLOSE                   102

#endif /* AVFILTER_MOVIE_ASYNC_H */

