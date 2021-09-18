/*
 * Filter graphs to bad ASCII-art
 * Copyright (c) 2012 Nicolas George
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

#include <string.h>

#include "libavutil/channel_layout.h"
#include "libavutil/bprint.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "filters.h"
#include "internal.h"

static int print_link_prop(AVBPrint *buf, AVFilterLink *link)
{
    char *format;
    char layout[64];
    AVBPrint dummy_buffer = { 0 };

    if (!buf)
        buf = &dummy_buffer;
    switch (link->type) {
        case AVMEDIA_TYPE_VIDEO:
            format = av_x_if_null(av_get_pix_fmt_name(link->format), "?");
            av_bprintf(buf, "[%dx%d %d:%d %s]", link->w, link->h,
                    link->sample_aspect_ratio.num,
                    link->sample_aspect_ratio.den,
                    format);
            break;

        case AVMEDIA_TYPE_AUDIO:
            av_get_channel_layout_string(layout, sizeof(layout),
                                         link->channels, link->channel_layout);
            format = av_x_if_null(av_get_sample_fmt_name(link->format), "?");
            av_bprintf(buf, "[%dHz %s:%s]",
                       (int)link->sample_rate, format, layout);
            break;

        default:
            av_bprintf(buf, "?");
            break;
    }
    return buf->len;
}

static void avfilter_graph_dump_to_buf(AVBPrint *buf, AVFilterGraph *graph)
{
    unsigned i, j, x, e;

    for (i = 0; i < graph->nb_filters; i++) {
        AVFilterContext *filter = graph->filters[i];
        unsigned max_src_name = 0, max_dst_name = 0;
        unsigned max_in_name  = 0, max_out_name = 0;
        unsigned max_in_fmt   = 0, max_out_fmt  = 0;
        unsigned width, height, in_indent;
        unsigned lname = strlen(filter->name);
        unsigned ltype = strlen(filter->filter->name);

        for (j = 0; j < filter->nb_inputs; j++) {
            AVFilterLink *l = filter->inputs[j];
            unsigned ln = strlen(l->src->name) + 1 + strlen(l->srcpad->name);
            max_src_name = FFMAX(max_src_name, ln);
            max_in_name = FFMAX(max_in_name, strlen(l->dstpad->name));
            max_in_fmt = FFMAX(max_in_fmt, print_link_prop(NULL, l));
        }
        for (j = 0; j < filter->nb_outputs; j++) {
            AVFilterLink *l = filter->outputs[j];
            unsigned ln = strlen(l->dst->name) + 1 + strlen(l->dstpad->name);
            max_dst_name = FFMAX(max_dst_name, ln);
            max_out_name = FFMAX(max_out_name, strlen(l->srcpad->name));
            max_out_fmt = FFMAX(max_out_fmt, print_link_prop(NULL, l));
        }
        in_indent = max_src_name + max_in_name + max_in_fmt;
        in_indent += in_indent ? 4 : 0;
        width = FFMAX(lname + 2, ltype + 4);
        height = FFMAX3(2, filter->nb_inputs, filter->nb_outputs);
        av_bprint_chars(buf, ' ', in_indent);
        av_bprintf(buf, "+");
        av_bprint_chars(buf, '-', width);
        av_bprintf(buf, "+\n");
        for (j = 0; j < height; j++) {
            unsigned in_no  = j - (height - filter->nb_inputs ) / 2;
            unsigned out_no = j - (height - filter->nb_outputs) / 2;

            /* Input link */
            if (in_no < filter->nb_inputs) {
                AVFilterLink *l = filter->inputs[in_no];
                e = buf->len + max_src_name + 2;
                av_bprintf(buf, "%s:%s", l->src->name, l->srcpad->name);
                av_bprint_chars(buf, '-', e - buf->len);
                e = buf->len + max_in_fmt + 2 +
                    max_in_name - strlen(l->dstpad->name);
                print_link_prop(buf, l);
                av_bprint_chars(buf, '-', e - buf->len);
                av_bprintf(buf, "%s", l->dstpad->name);
            } else {
                av_bprint_chars(buf, ' ', in_indent);
            }

            /* Filter */
            av_bprintf(buf, "|");
            if (j == (height - 2) / 2) {
                x = (width - lname) / 2;
                av_bprintf(buf, "%*s%-*s", x, "", width - x, filter->name);
            } else if (j == (height - 2) / 2 + 1) {
                x = (width - ltype - 2) / 2;
                av_bprintf(buf, "%*s(%s)%*s", x, "", filter->filter->name,
                        width - ltype - 2 - x, "");
            } else {
                av_bprint_chars(buf, ' ', width);
            }
            av_bprintf(buf, "|");

            /* Output link */
            if (out_no < filter->nb_outputs) {
                AVFilterLink *l = filter->outputs[out_no];
                unsigned ln = strlen(l->dst->name) + 1 +
                              strlen(l->dstpad->name);
                e = buf->len + max_out_name + 2;
                av_bprintf(buf, "%s", l->srcpad->name);
                av_bprint_chars(buf, '-', e - buf->len);
                e = buf->len + max_out_fmt + 2 +
                    max_dst_name - ln;
                print_link_prop(buf, l);
                av_bprint_chars(buf, '-', e - buf->len);
                av_bprintf(buf, "%s:%s", l->dst->name, l->dstpad->name);
            }
            av_bprintf(buf, "\n");
        }
        av_bprint_chars(buf, ' ', in_indent);
        av_bprintf(buf, "+");
        av_bprint_chars(buf, '-', width);
        av_bprintf(buf, "+\n");
        av_bprintf(buf, "\n");
    }
}

char *avfilter_graph_dump(AVFilterGraph *graph, const char *options)
{
    AVBPrint buf;
    char *dump = NULL;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_COUNT_ONLY);
    avfilter_graph_dump_to_buf(&buf, graph);
    av_bprint_init(&buf, buf.len + 1, buf.len + 1);
    avfilter_graph_dump_to_buf(&buf, graph);
    av_bprint_finalize(&buf, &dump);
    return dump;
}

static void graph_link_dump(AVBPrint *buf, AVFilterContext *cur, AVFilterLink *link)
{
    char *format;
    char tmp[64];

    if (!link) {
        av_bprintf(buf, "%73s", "");
        return;
    }

    switch (link->type) {
        case AVMEDIA_TYPE_VIDEO:
            format = av_x_if_null(av_get_pix_fmt_name(link->format), "?");
            av_bprintf(buf, "wh:%-10d|%-10d ra:%-4d|%-4d %-8s ",
                       link->w, link->h,
                       link->sample_aspect_ratio.num,
                       link->sample_aspect_ratio.den,
                       format);
            break;

        case AVMEDIA_TYPE_AUDIO:
            av_get_channel_layout_string(tmp, sizeof(tmp),
                                         link->channels, link->channel_layout);
            format = av_x_if_null(av_get_sample_fmt_name(link->format), "?");
            av_bprintf(buf, "fmt:%-4s sr:%-6d cl:%-12s ",
                       format, link->sample_rate, tmp);
            break;

        default:
            av_bprintf(buf, "?");
            break;
    }

    av_bprintf(buf, "st:%d wn:%d cnt:%-8lld cur:%d|%-8d ", !ff_outlink_get_status(link),
               ff_outlink_frame_wanted(link), link->frame_count_in,
               ff_inlink_queued_frames(link), ff_inlink_queued_samples(link));
}

static void graph_filter_dump(AVBPrint *buf, AVFilterContext *cur)
{
    char tmp[64];
    int i = 0;

    do {
        AVFilterLink *link = NULL;
        AVFilterContext *next = NULL;

        if (cur->nb_outputs) {
            link = cur->outputs[i];
            next = link->dst;
        }

        av_bprintf(buf, "%-24s -> %-24s", cur->name, next ? next->name : "NULL");
        graph_link_dump(buf, cur, link);

        av_bprintf(buf, "elap:%-4d|%-4d|%-5d ",
                cur->elapsed, cur->elapsed_min, cur->elapsed_max);

        if (avfilter_process_command(cur, "dump", NULL, tmp, sizeof(tmp), 0) >= 0)
            av_bprintf(buf, "ex:%s\n", tmp);
        else
            av_bprintf(buf, "ex:N/A\n");

        if (next && !(next->nb_inputs > 1 || next->nb_outputs > 1))
            graph_filter_dump(buf, next);
    } while (++i < cur->nb_outputs);
}

char *avfilter_graph_dump_ext(AVFilterGraph *graph, const char *options)
{
    AVFilterContext *cur;
    AVBPrint buf;
    char *dump = NULL;
    int i;

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    for (i = 0; i < graph->nb_filters; i++) {
        cur = graph->filters[i];

        if (!cur->nb_inputs || cur->nb_inputs > 1 || cur->nb_outputs > 1)
            graph_filter_dump(&buf, cur);
    }

    av_bprint_finalize(&buf, &dump);
    return dump;
}
