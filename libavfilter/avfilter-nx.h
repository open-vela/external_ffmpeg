
/*
 * filter layer
 * Copyright (c) 2007 Bobby Bingham
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
#ifndef AVFILTER_AVFILTER_NX_H
#define AVFILTER_AVFILTER_NX_H
//#define AVFILTER_CMD_FLAG_ONE       1 ///< Stop once a filter understood the command (for target=all for example), fast filters are favored automatically
//#define AVFILTER_CMD_FLAG_FAST      2 ///< Only execute command when its fast (like a video out that supports contrast adjustment in hw)
#define AVFILTER_CMD_FLAG_REVERSE   4 ///< Reverse forward command to source filter (default to sink filter)
//#define AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL (1 << 17)

/**
 * Support cmds get_pollfd & poll_available
 */
#define AVFILTER_FLAG_SUPPORT_POLL          (1 << 18)

typedef struct AVOptionRanges AVOptionRanges;
/*
 * Get a list of allowed ranges for the given option.
 *
 * The result must be freed with av_opt_free_ranges.
 *
 * @return number of compontents returned on success, a negative errro code otherwise
 */
int av_opt_query_ranges2(AVOptionRanges **ranges_arg, void *obj, void *udata, const char *key, int flags);

#endif /* AVFILTER_AVFILTER_NX_H */
