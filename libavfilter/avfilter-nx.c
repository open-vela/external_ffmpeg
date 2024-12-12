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
#include "avfilter-nx.h"
#include <libavutil/opt.h>

#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"

int av_opt_query_ranges2(AVOptionRanges **ranges_arg, void *obj, void *udata, const char *key, int flags)
{
    int ret;
    const AVClass *c = *(AVClass **)obj;
    int (*callback)(AVOptionRanges **, void *obj, const char *key, int flags) = c->query_ranges;

    if (!callback)
        callback = av_opt_query_ranges_default;

    ret = callback(ranges_arg, udata, key, flags);
    if (ret >= 0) {
        if (!(flags & AV_OPT_MULTI_COMPONENT_RANGE))
            ret = 1;
        (*ranges_arg)->nb_components = ret;
    }
    return ret;
}
