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

#ifndef AVFILTER_AMIX_H
#define AVFILTER_AMIX_H

#include <stdbool.h>

typedef struct AVFrame AVFrame;
typedef struct AVFilterLink AVFilterLink;
typedef struct AVFilterContext AVFilterContext;
typedef struct AMixContext AMixContext;

bool ff_amix_input_empty(AMixContext *s, AVFilterLink *link);
bool ff_amix_input_want(AMixContext *s, AVFilterLink *link);
int ff_amix_input_write(AMixContext *s, AVFilterLink *link);
bool ff_amix_input_write_down(AMixContext *s, AVFilterLink *link);
int ff_amix_read(AMixContext *s, AVFrame **oframe);
AMixContext *ff_amix_alloc(int sample_rate, int format, int channels);
bool ff_amix_blocked(AMixContext *s);
void ff_amix_free(AMixContext *s);
int ff_amix_set_frame_size(AMixContext *s, int frame_size);

#endif /* AVFILTER_AMIX_H */