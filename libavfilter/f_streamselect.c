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

#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct StreamSelectContext {
    const AVClass *class;
    int nb_inputs;
    char *map_str;
    int *map;
    int nb_map;
    int is_audio;
    int prevent_eof;
} StreamSelectContext;

#define OFFSET(x) offsetof(StreamSelectContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
#define TFLAGS AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption streamselect_options[] = {
    { "inputs",  "number of input streams",           OFFSET(nb_inputs),  AV_OPT_TYPE_INT,    {.i64=1},    1, INT_MAX,  .flags=FLAGS },
    { "map",     "input indexes to remap to outputs", OFFSET(map_str),    AV_OPT_TYPE_STRING, {.str=NULL},              .flags=TFLAGS },
    { "prevent_eof",  "prevent the reverse transmission of EOF", OFFSET(prevent_eof),  AV_OPT_TYPE_INT, {.i64=0}, 0, INT_MAX, .flags=FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS_EXT(streamselect, "(a)streamselect", streamselect_options);

static int activate(AVFilterContext *ctx)
{
    StreamSelectContext *s = ctx->priv;
    int i, j, ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        AVFrame *frame = NULL;
        bool first = true;
        bool request = true;
        bool eof = true;
        int64_t rpts;
        int status;

        ff_inlink_acknowledge_status(ctx->inputs[i], &status, &rpts);
        if (!status) {
            ret = ff_inlink_consume_frame(ctx->inputs[i], &frame);
            if (ret < 0)
                return ret;
        }

        for (j = 0; j < s->nb_map; j++) {
            if (ff_outlink_get_status(ctx->outputs[j])) {
                if (s->map[j] >= 0 && s->prevent_eof)
                    eof = false;
                continue;
            }

            if (s->map[j] < 0)
                ff_outlink_set_status(ctx->outputs[j], AVERROR_EOF, AV_NOPTS_VALUE);
            else if (s->map[j] == i) {
                if (status) {
                    ff_outlink_set_status(ctx->outputs[j], AVERROR_EOF, AV_NOPTS_VALUE);
                } else if (frame) {
                    if (first) { // do not clone at fisrt sending.
                        ret = ff_filter_frame(ctx->outputs[j], frame);
                        if (ret < 0)
                            return ret;
                        first = false;
                    } else {
                        AVFrame *out = av_frame_clone(frame);

                        if (!out) {
                            av_frame_free(&frame);
                            return AVERROR(ENOMEM);
                        }

                        ret = ff_filter_frame(ctx->outputs[j], out);
                        if (ret < 0)
                            return ret;
                    }
                    ff_inlink_acknowledge_status(ctx->inputs[i], &status, &rpts);
                    if (status)
                        ff_outlink_set_status(ctx->outputs[j], AVERROR_EOF, AV_NOPTS_VALUE);
                } else if (!ff_outlink_frame_wanted(ctx->outputs[j])) {
                    request = false;
                }

                eof = false;
            }
        }

        if (!status) {
            if (eof)
                ff_inlink_set_status(ctx->inputs[i], AVERROR_EOF);
            else if (!frame && request)
                ff_inlink_request_frame(ctx->inputs[i]);
        }

        if (first) // frame not taken by ouputs
            av_frame_free(&frame);
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    StreamSelectContext *s = ctx->priv;
    const int outlink_idx = FF_OUTLINK_IDX(outlink);
    const int inlink_idx  = s->map[outlink_idx];

    if (inlink_idx < 0)
        return 0;

    if (outlink->type == AVMEDIA_TYPE_VIDEO) {
        AVFilterLink *inlink = ctx->inputs[inlink_idx];

        outlink->w = inlink->w;
        outlink->h = inlink->h;
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
        outlink->frame_rate = inlink->frame_rate;
    }

    return 0;
}

static int parse_definition(AVFilterContext *ctx, int nb_pads, int is_input, int is_audio)
{
    const char *padtype = is_input ? "in" : "out";
    int i = 0, ret = 0;

    for (i = 0; i < nb_pads; i++) {
        AVFilterPad pad = { 0 };

        pad.type = is_audio ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;

        pad.name = av_asprintf("%sput%d", padtype, i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        av_log(ctx, AV_LOG_DEBUG, "Add %s pad %s\n", padtype, pad.name);

        if (is_input) {
            ret = ff_append_inpad_free_name(ctx, &pad);
        } else {
            pad.config_props  = config_output;
            ret = ff_append_outpad_free_name(ctx, &pad);
        }
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int parse_mapping(AVFilterContext *ctx, const char *map)
{
    StreamSelectContext *s = ctx->priv;
    int *new_map;
    int new_nb_map = 0;

    if (!map) {
        av_log(ctx, AV_LOG_ERROR, "mapping definition is not set\n");
        return AVERROR(EINVAL);
    }

    new_map = av_calloc(ctx->nb_outputs, sizeof(*new_map));
    if (!new_map)
        return AVERROR(ENOMEM);

    while (1) {
        char *p;
        const int n = strtol(map, &p, 0);

        av_log(ctx, AV_LOG_DEBUG, "n=%d map=%p p=%p\n", n, map, p);

        if (map == p)
            break;
        map = p;

        if (new_nb_map >= ctx->nb_outputs) {
            av_log(ctx, AV_LOG_ERROR, "Unable to map more than the %d "
                   "output pads available\n", ctx->nb_outputs);
            av_free(new_map);
            return AVERROR(EINVAL);
        }

        if (n >= s->nb_inputs) {
            av_log(ctx, AV_LOG_ERROR, "Input stream index %d doesn't exist "
                   "(there is only %d input streams defined)\n",
                   n, s->nb_inputs);
            av_free(new_map);
            return AVERROR(EINVAL);
        }

        av_log(ctx, AV_LOG_VERBOSE, "Map input stream %d to output stream %d\n", n, new_nb_map);
        new_map[new_nb_map++] = n;
    }

    if (!new_nb_map) {
        av_log(ctx, AV_LOG_ERROR, "invalid mapping\n");
        av_free(new_map);
        return AVERROR(EINVAL);
    }

    av_freep(&s->map);
    s->map = new_map;
    s->nb_map = new_nb_map;

    av_log(ctx, AV_LOG_VERBOSE, "%d map set\n", s->nb_map);

    return 0;
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                           char *res, int res_len, int flags)
{
    StreamSelectContext *s = ctx->priv;
    bool need_reconfig = false;
    int pos = 0, ret, i;

    if (!strcmp(cmd, "map")) {
        int ret = parse_mapping(ctx, args);
        if (ret < 0)
            return ret;

        for (i = 0; i < s->nb_map; i++)
            if (s->map[i] < 0)
                ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
            else
                need_reconfig = true;

        if (need_reconfig) {
            ret = avfilter_graph_reconfig(ctx->graph, ctx);
            if (ret >= 0) {
                for (int i = 0; i < ctx->nb_outputs; i++)
                    if (ctx->outputs[i]->incfg.formats)
                        ctx->outputs[i]->frame_wanted_out = 1;
            }
        }
        ff_filter_set_ready(ctx, 100);

        return ret;
    } else if (!strcmp(cmd, "dump")) {
        if (s->map) {
            ret = snprintf(res, res_len, "map:");
            pos += ret;

            for (i = 0; i < s->nb_map; i++) {
                ret = snprintf(res + pos, res_len - pos, "%d ", s->map[i]);
                pos += ret;

                if (pos >= res_len)
                    break;
            }
        }

        return 0;
    }

    return AVERROR(ENOSYS);
}

static int forward_command(AVFilterContext *ctx, int pad_idx, const char* target, const char *cmd,
                           const char *arg, char *res, int res_len, int flags)
{
    StreamSelectContext *s = ctx->priv;
    int i, ret = AVERROR(ENOSYS);

    if (flags & AVFILTER_CMD_FLAG_REVERSE) {
        /* Forward to the inlink that the outlink picks. */
        if (s->map[pad_idx] >= 0) {
            return avfilter_forward_command(ctx, s->map[pad_idx], target, cmd, arg, res, res_len, flags);
        }
    } else {
        /* Forward to all outlinks that pick the inlink. */
        for (i = 0; i < ctx->nb_outputs; i++) {
            if (s->map[i] == pad_idx) {
                ret = avfilter_forward_command(ctx, i, target, cmd, arg, res, res_len, flags);
                if (ret != AVERROR(ENOSYS)) {
                    if ((flags & AVFILTER_CMD_FLAG_ONE) || ret < 0)
                        return ret;
                }
            }
        }
    }

    return ret;
}

static av_cold int init(AVFilterContext *ctx)
{
    StreamSelectContext *s = ctx->priv;
    int ret, nb_outputs = 0;
    char *map = s->map_str;

    if (!strcmp(ctx->filter->name, "astreamselect"))
        s->is_audio = 1;

    for (; map;) {
        char *p;

        strtol(map, &p, 0);
        if (map == p)
            break;
        nb_outputs++;
        map = p;
    }

    if ((ret = parse_definition(ctx, s->nb_inputs, 1, s->is_audio)) < 0 ||
        (ret = parse_definition(ctx, nb_outputs, 0, s->is_audio)) < 0)
        return ret;

    av_log(ctx, AV_LOG_DEBUG, "Configured with %d inpad and %d outpad\n",
           ctx->nb_inputs, ctx->nb_outputs);

    return parse_mapping(ctx, s->map_str);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    StreamSelectContext *s = ctx->priv;

    av_freep(&s->map);
}

static int query_formats_ref(AVFilterLink *link, bool out,
                             AVFilterFormats *codecs,
                             AVFilterFormats *formats,
                             AVFilterFormats *rates,
                             AVFilterChannelLayouts *layouts)
{
    int ret;

    if (out ? !link->outcfg.codecs : !link->incfg.codecs) {
        ret = ff_formats_ref(codecs, out ? &link->outcfg.codecs: &link->incfg.codecs);
        if (ret < 0)
            return ret;
    }

    if (out ? !link->outcfg.formats : !link->incfg.formats) {
        ret = ff_formats_ref(formats, out ? &link->outcfg.formats : &link->incfg.formats);
        if (ret < 0)
            return ret;
    }

    if (link->type == AVMEDIA_TYPE_AUDIO) {
        if (out ? !link->outcfg.samplerates : !link->incfg.samplerates) {
            ret = ff_formats_ref(rates, out ? &link->outcfg.samplerates : &link->incfg.samplerates);
            if (ret < 0)
                return ret;
        }

        if (out ? !link->outcfg.channel_layouts : !link->incfg.channel_layouts) {
            ret = ff_channel_layouts_ref(layouts, out ? &link->outcfg.channel_layouts : &link->incfg.channel_layouts);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static void query_formats_unref(AVFilterLink *link, bool out)
{
    if (out) {
        ff_formats_unref(&link->outcfg.codecs);
        ff_formats_unref(&link->outcfg.formats);
        ff_formats_unref(&link->outcfg.samplerates);
        ff_channel_layouts_unref(&link->outcfg.channel_layouts);
    } else {
        ff_formats_unref(&link->incfg.codecs);
        ff_formats_unref(&link->incfg.formats);
        ff_formats_unref(&link->incfg.samplerates);
        ff_channel_layouts_unref(&link->incfg.channel_layouts);
    }
}

static int query_formats(AVFilterContext *ctx)
{
    StreamSelectContext *s = ctx->priv;
    AVFilterFormats *codecs  = NULL;
    AVFilterFormats *formats = NULL;
    AVFilterFormats *rates   = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    int ret = 0;
    int i, j;

    for (i = 0; i < ctx->nb_inputs; i++) {
        bool eof = true;

        for (j = 0; j < s->nb_map; j++) {
            if (s->map[j] < 0)
                query_formats_unref(ctx->outputs[j], false);
            else if (s->map[j] == i) {
                if (ctx->outputs[j]->incfg.formats) {
                    codecs  = ctx->outputs[j]->incfg.codecs;
                    formats = ctx->outputs[j]->incfg.formats;
                    rates   = ctx->outputs[j]->incfg.samplerates;
                    layouts = ctx->outputs[j]->incfg.channel_layouts;
                }
                eof = false;
            }
        }

        if (eof) {
            query_formats_unref(ctx->inputs[i], true);
            continue;
        }

        if (!formats) {
            if (ctx->inputs[i]->outcfg.formats) {
                codecs  = ctx->inputs[i]->outcfg.codecs;
                formats = ctx->inputs[i]->outcfg.formats;
                rates   = ctx->inputs[i]->outcfg.samplerates;
                layouts = ctx->inputs[i]->outcfg.channel_layouts;
            } else {
                codecs  = ff_all_codecs(ctx->inputs[0]->type);
                formats = ff_all_formats(ctx->inputs[0]->type);
                rates   = ff_all_samplerates();
                layouts = ff_all_channel_counts();
            }
        }

        for (j = 0; j < s->nb_map; j++) {
            if (s->map[j] == i) {
                ret = query_formats_ref(ctx->outputs[j], false, codecs, formats, rates, layouts);
                if (ret < 0)
                    goto out;
            }
        }

        ret = query_formats_ref(ctx->inputs[i], true, codecs, formats, rates, layouts);

out:
        ff_formats_unref(&codecs);
        ff_formats_unref(&formats);
        ff_formats_unref(&rates);
        ff_channel_layouts_unref(&layouts);

        if (ret < 0)
            return ret;
    }

    return 0;
}

static int sanitize_formats(AVFilterContext *ctx)
{
    StreamSelectContext *s = ctx->priv;
    bool changed = false;
    int i, j;

    for (i = 0; i < ctx->nb_inputs; i++) {
        bool used = false;

        for (j = 0; j < s->nb_map; j++) {
            if (s->map[j] == i && (ctx->outputs[j]->incfg.formats ||
               (ctx->inputs[i]->outcfg.formats && s->prevent_eof))) {
                used = true;
                break;
            }
        }

        if (ctx->inputs[i]->outcfg.formats && !used) {
            query_formats_unref(ctx->inputs[i], true);
            changed = true;
        } else if (!ctx->inputs[i]->outcfg.formats && used) {
            for (j = 0; j < s->nb_map; j++) {
                if (s->map[j] == i)
                    query_formats_unref(ctx->outputs[j], false);
            }
            changed = true;
        }
    }

    return changed ? AVERROR(EAGAIN) : 0;
}

const AVFilter ff_vf_streamselect = {
    .name             = "streamselect",
    .description      = NULL_IF_CONFIG_SMALL("Select video streams"),
    .init             = init,
    FILTER_QUERY_FUNC(query_formats),
    .sanitize_formats = sanitize_formats,
    .process_command  = process_command,
    .forward_command  = forward_command,
    .uninit           = uninit,
    .activate         = activate,
    .priv_size        = sizeof(StreamSelectContext),
    .priv_class       = &streamselect_class,
    .flags            = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};

const AVFilter ff_af_astreamselect = {
    .name             = "astreamselect",
    .description      = NULL_IF_CONFIG_SMALL("Select audio streams"),
    .priv_class       = &streamselect_class,
    .init             = init,
    FILTER_QUERY_FUNC(query_formats),
    .sanitize_formats = sanitize_formats,
    .process_command  = process_command,
    .forward_command  = forward_command,
    .uninit           = uninit,
    .activate         = activate,
    .priv_size        = sizeof(StreamSelectContext),
    .flags            = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
