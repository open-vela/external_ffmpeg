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

#ifndef AVFILTER_MAPPING_H
#define AVFILTER_MAPPING_H

#include <stddef.h>
#include <stdlib.h>
#include "libavutil/error.h"
#include "libavutil/mem.h"

#define ROUTE_OFF 0
#define ROUTE_ON 1

/**
 * @file
 * control routing for src filter
 */

/**
 * Parse the mapping definition.
 *
 * @param map_str      The mapping definition string.
 * @param map          Pointer to an array that will hold the parsed mapping relationships.
 *                     The array will be allocated by this function and should be freed
 *                     by the caller using av_freep().
 * @param nb_map       The number of mappings expected in the map array.
 * @return             0 on success, a negative AVERROR code on error.
 */
int avfilter_parse_mapping(const char *map_str, int **map, int nb_map);

#endif /* AVFILTER_MAPPING_H */
