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

#include "mapping.h"

/**
 * Parse a mapping string and build an integer array.
 *
 * @param map_str    Input string to parse (must not be NULL)
 * @param map        Output array pointer
 * @param nb_map     Maximum number of elements to parse (must be > 0 )
 * @return           0 on success, negative error code on failure
 *                   - AVERROR(EINVAL): Invalid input parameters
 *                   - AVERROR(ENOMEM): Memory allocation failed
 *                   - AVERROR(ERANGE): Integer overflow in parsing
 */
int avfilter_parse_mapping(const char *map_str, int **map, int nb_map)
{
    int *new_map = NULL;
    int new_nb_map = 0;

    if (!map_str || nb_map <= 0)
        return AVERROR(EINVAL);

    new_map = av_calloc(nb_map, sizeof(*new_map));
    if (!new_map)
        return AVERROR(ENOMEM);

    while (1) {
        char *p;
        int val = strtol(map_str, &p, 0);
        if (map_str == p)
            break;
        map_str = p;

        if (val != ROUTE_ON && val != ROUTE_OFF) {
            av_freep(&new_map);
            return AVERROR(ERANGE);
        }

        if (new_nb_map >= nb_map) {
            av_freep(&new_map);
            return AVERROR(EINVAL);
        }

        new_map[new_nb_map++] = val;
    }

    if (!new_nb_map) {
        av_freep(&new_map);
        return AVERROR(EINVAL);
    }

    av_freep(map);

    *map = new_map;
    return 0;
}
