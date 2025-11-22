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

/**
 * @file
 * AVfilter graph
 */

#include <libavutil/channel_layout.h>
#include <libavutil/avstring.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "avfilter_internal.h"
#include "buffersink.h"
#include "buffersrc.h"
#include "avfilter.h"
#include "formats.h"
#include "mapping.h"

#include <sys/queue.h>

#define ROUTE_OFF 0
#define ROUTE_ON 1

typedef struct SubCmd {
    SIMPLEQ_ENTRY(SubCmd) entry;
    char                  *cmd;
} SubCmd;

SIMPLEQ_HEAD(SubCmdQueue, SubCmd);

typedef struct SubGraphInstance {
    AVFilterGraph   *graph;
    AVFilterContext *src_filter;
    AVFilterContext *sink_filter;
} SubGraphInstance;

typedef struct SubGraphPriv {
    const AVClass *class;

    SubGraphInstance graph_inst;
    struct SubCmdQueue cmd_queue;

    int nb_inputs;
    int nb_outputs;
    char *map_str;
    int *map;

    char *graph_desc;

    bool first_frame_sent;
} SubGraphPriv;

/**
 * @struct SubGraphFormats
 * Configuration for supported audio formats in subgraph
 */
typedef struct SubGraphFormats {
    int sample_rate;
    enum AVSampleFormat format;
    AVChannelLayout ch_layout;
} SubGraphFormats;

static int asubgraph_init_instance(const AVFilterContext *ctx, SubGraphInstance *inst,
                                   const SubGraphFormats *src_fmt,
                                   const SubGraphFormats *sink_fmt)
{
    AVFilterInOut *outputs = NULL, *inputs = NULL;
    AVFilterContext *src_filter, *sink_filter;
    SubGraphPriv *priv = ctx->priv;
    void *logger = (void *)ctx;
    char ch_layout_str[16];
    AVFilterGraph *graph;
    char tmp[64];
    int ret;

    graph = avfilter_graph_alloc();
    if (!graph)
        return AVERROR(ENOMEM);

    // create src filter
    ret = av_channel_layout_describe(&src_fmt->ch_layout, ch_layout_str,
                                     sizeof(ch_layout_str));
    if (ret < 0) {
        av_log(logger, AV_LOG_ERROR, "failed to describe src_fmt channel layout\n");
        goto fail;
    }

    snprintf(tmp, sizeof(tmp), "sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             src_fmt->sample_rate, av_get_sample_fmt_name(src_fmt->format), ch_layout_str);

    ret = avfilter_graph_create_filter(&src_filter, avfilter_get_by_name("abuffer"),
                                       "abuffer", tmp, NULL, graph);
    if (ret < 0) {
        av_log(logger, AV_LOG_ERROR, "cannot create audio source filter ret %d.\n", ret);
        goto fail;
    }

    // create sink filter
    if (sink_fmt) {
        memset(tmp, 0, sizeof(tmp));
        memset(ch_layout_str, 0, sizeof(ch_layout_str));
        ret = av_channel_layout_describe(&sink_fmt->ch_layout, ch_layout_str,
                                         sizeof(ch_layout_str));
        if (ret < 0) {
            av_log(logger, AV_LOG_ERROR, "failed to describe sink_fmt channel layout\n");
            goto fail;
        }
        snprintf(tmp, sizeof(tmp), "samplerates=%d:sample_formats=%s:channel_layouts=%s",
                 sink_fmt->sample_rate, av_get_sample_fmt_name(sink_fmt->format), ch_layout_str);
    }

    ret = avfilter_graph_create_filter(&sink_filter, avfilter_get_by_name("abuffersink"),
                                       "abuffersink", sink_fmt ? tmp : NULL, NULL, graph);
    if (ret < 0) {
        av_log(logger, AV_LOG_ERROR, "cannot create audio sink filter ret %d.\n", ret);
        goto fail;
    }

    outputs = avfilter_inout_alloc();
    inputs  = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = src_filter;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = sink_filter;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;
    ret = avfilter_graph_parse_ptr(graph, priv->graph_desc, &inputs, &outputs, NULL);
    if (ret < 0) {
        av_log(logger, AV_LOG_ERROR, "parsing subgraph error %d\n", ret);
        goto fail;
    }

    inst->graph       = graph;
    inst->src_filter  = src_filter;
    inst->sink_filter = sink_filter;
    return 0;

fail:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    return ret;
}

static void asubgraph_uninit_instance(SubGraphInstance *inst)
{
    if (!inst || !inst->graph)
        return;

    avfilter_graph_free(&inst->graph);
    inst->src_filter  = NULL;
    inst->sink_filter = NULL;
}

static inline bool asubgraph_instance_is_inited(SubGraphInstance *inst)
{
    return (inst && inst->graph) ? true : false;
}

static void asubgraph_free_cmd(SubCmd **cmd)
{
    if (!cmd || !*cmd)
        return;

    if ((*cmd)->cmd)
        av_free((*cmd)->cmd);

    av_freep(cmd);
}

static int asubgraph_enqueue_cmd(struct SubCmdQueue *cmd_queue, const char *cmd)
{
    SubCmd *new_cmd;

    new_cmd = av_mallocz(sizeof(SubCmd));
    if (!new_cmd)
        return AVERROR(ENOMEM);

    new_cmd->cmd = av_strdup(cmd);
    if (!new_cmd->cmd) {
        av_free(new_cmd);
        return AVERROR(ENOMEM);
    }

    SIMPLEQ_INSERT_TAIL(cmd_queue, new_cmd, entry);
    return 0;
}

static int asubgraph_dump(AVFilterContext *ctx)
{
    SubGraphPriv *priv = ctx->priv;
    char* dump;

    if (!asubgraph_instance_is_inited(&priv->graph_inst))
        return AVERROR(EINVAL);

    dump = avfilter_graph_dump(priv->graph_inst.graph, NULL);
    if (dump != NULL) {
        av_log(ctx, AV_LOG_INFO, "%s dump:\n%s\n", ctx->name, dump);
        av_free(dump);
    } else {
        av_log(ctx, AV_LOG_ERROR, "%s unable to dump\n", ctx->name);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int asubgraph_process_subcmd(AVFilterContext *ctx, const char *args,
                                    char *res, int res_len, int flags)
{
    char *p, *target, *sub_cmd, *sub_args;
    SubGraphPriv *priv = ctx->priv;
    char *saveptr = NULL;
    int ret;
    int i;

    if (!args)
        return AVERROR(EINVAL);

    // when graph_inst is not initialized, enqueue the cmd to cmd_queue
    if (!asubgraph_instance_is_inited(&priv->graph_inst)) {
        if (!priv)
            return AVERROR(EINVAL);

        av_log(ctx, AV_LOG_INFO, "%s graph_inst is not init, pending sub_cmd: %s\n",
               ctx->name, args);
        ret = asubgraph_enqueue_cmd(&priv->cmd_queue, args);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "enqueue_cmd error %d\n", ret);
            return ret;
        }

        return 0;
    }

    p = av_strdup(args);
    if (!p)
        return AVERROR(ENOMEM);

    target = strtok_r(p, ":", &saveptr);
    sub_cmd = target ? strtok_r(NULL, ":", &saveptr) : NULL;
    sub_args = sub_cmd ? strtok_r(NULL, ":", &saveptr) : NULL;

    if (!target || !sub_cmd || !sub_args) {
        av_log(ctx, AV_LOG_ERROR, "invalid format for sub_cmd: %s\n", args);
        ret = AVERROR(EINVAL);
        goto end;
    }

    av_log(ctx, AV_LOG_INFO, "process sub_cmd: %s %s %s.\n", target, sub_cmd, sub_args);

    for (i = 0; i < priv->graph_inst.graph->nb_filters; i++) {
        AVFilterContext *filter = priv->graph_inst.graph->filters[i];
        if ((filter->name && !strcmp(target, filter->name))
            || !strcmp(target, filter->filter->name)) {
            ret = avfilter_process_command(filter, sub_cmd, sub_args, res, res_len, flags);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "error executing filter(%s) command %s %s %d\n",
                       target, sub_cmd, args, ret);
            }
            goto end;
        }
    }

    av_log(ctx, AV_LOG_ERROR, "filter %s not found\n", target);
    ret = AVERROR(ENOENT);

end:
    av_freep(&p);
    return ret;
}

static int asubgraph_query_alg_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in,
                                       AVFilterFormatsConfig **cfg_out)
{
    AVFilterFormatsConfig *alg_cfg_in, *alg_cfg_out, *tmp_cfg;
    AVFilterContext *first_alg, *last_alg;
    SubGraphPriv *priv = ctx->priv;
    SubGraphInstance inst = { 0 };
    void *logger = (void *)ctx;
    int ret;
    int i;

    SubGraphFormats default_fmt = {
        .format = AV_SAMPLE_FMT_S16,
        .sample_rate = 44100,
        .ch_layout = AV_CHANNEL_LAYOUT_STEREO,
    };

    av_log(logger, AV_LOG_INFO, "graph_inst query_formats: %s.\n", priv->graph_desc);

    ret = asubgraph_init_instance(ctx, &inst, &default_fmt, NULL);
    if (ret < 0) {
        av_log(logger, AV_LOG_ERROR, "cannot create graph_inst %d.\n", ret);
        goto end;
    }

    first_alg = inst.src_filter->outputs[0]->dst;
    last_alg  = inst.sink_filter->inputs[0]->src;
    if (first_alg->filter->formats_state != FF_FILTER_FORMATS_QUERY_FUNC2 ||
        last_alg->filter->formats_state != FF_FILTER_FORMATS_QUERY_FUNC2) {
        av_log(logger, AV_LOG_ERROR, " %s not support query_func2.\n",
            first_alg->filter->formats_state != FF_FILTER_FORMATS_QUERY_FUNC2 ?
            first_alg->name : last_alg->name);
        ret = AVERROR(EINVAL);
        goto end;
    }

    alg_cfg_in  = &first_alg->inputs[0]->outcfg;
    tmp_cfg     = &first_alg->outputs[0]->incfg;
    ret = first_alg->filter->formats.query_func2(first_alg, &alg_cfg_in, &tmp_cfg);
    if (ret < 0)
        goto end;

    tmp_cfg     = &last_alg->inputs[0]->outcfg;
    alg_cfg_out = &last_alg->outputs[0]->incfg;
    ret = last_alg->filter->formats.query_func2(last_alg, &tmp_cfg, &alg_cfg_out);
    if (ret < 0)
        goto end;

    for (i = 0; i < ctx->nb_inputs; i++) {
        ff_formats_unref(&cfg_in[i]->formats);
        ret = ff_formats_ref(alg_cfg_in->formats, &cfg_in[i]->formats);
        if (ret < 0)
            goto end;

        ff_formats_unref(&cfg_in[i]->samplerates);
        ret = ff_formats_ref(alg_cfg_in->samplerates, &cfg_in[i]->samplerates);
        if (ret < 0)
            goto end;

        ff_channel_layouts_unref(&cfg_in[i]->channel_layouts);
        ret = ff_channel_layouts_ref(alg_cfg_in->channel_layouts, &cfg_in[i]->channel_layouts);
        if (ret < 0)
            goto end;
    }

    for (i = 0; i < ctx->nb_outputs; i++) {
        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(alg_cfg_out->formats, &cfg_out[i]->formats);
        if (ret < 0)
            goto end;

        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(alg_cfg_out->samplerates, &cfg_out[i]->samplerates);
        if (ret < 0)
            goto end;

        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(alg_cfg_out->channel_layouts, &cfg_out[i]->channel_layouts);
        if (ret < 0)
            goto end;
    }

end:
    asubgraph_uninit_instance(&inst);
    return ret;
}

static void asubgraph_close(AVFilterContext *ctx)
{
    SubGraphPriv *priv = ctx->priv;

    priv->first_frame_sent = false;
    asubgraph_uninit_instance(&priv->graph_inst);
}

static int asubgraph_open(AVFilterContext *ctx, const SubGraphFormats src_fmt,
                          const SubGraphFormats sink_fmt)
{
    SubGraphPriv *priv = ctx->priv;
    SubCmd *cmd;
    int ret;

    av_log(ctx, AV_LOG_INFO, "%s graph_inst init parms: %d %d %d(src) %d %d %d(sink).\n",
           ctx->name, src_fmt.format, src_fmt.sample_rate, src_fmt.ch_layout.nb_channels,
           sink_fmt.format, sink_fmt.sample_rate, sink_fmt.ch_layout.nb_channels);

    // all format should be setted as vaild value
    if (src_fmt.ch_layout.nb_channels <= 0   || sink_fmt.ch_layout.nb_channels <= 0 ||
        src_fmt.sample_rate <= 0             || sink_fmt.sample_rate           <= 0 ||
        src_fmt.format <= AV_SAMPLE_FMT_NONE || sink_fmt.format <= AV_SAMPLE_FMT_NONE) {
        av_log(ctx, AV_LOG_ERROR, "invalid parameters.\n");
        return AVERROR(EINVAL);
    }

    ret = asubgraph_init_instance(ctx, &priv->graph_inst, &src_fmt, &sink_fmt);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot create audio graph_inst %d.\n", ret);
        goto fail;
    }

    while ((cmd = SIMPLEQ_FIRST(&priv->cmd_queue)) != NULL) {
        ret = asubgraph_process_subcmd(ctx, cmd->cmd, NULL, 0, 0);
        if (ret < 0)
            av_log(ctx, AV_LOG_ERROR, "error processing sub_cmd: %s, ret=%d\n", cmd->cmd, ret);

        SIMPLEQ_REMOVE_HEAD(&priv->cmd_queue, entry);
        asubgraph_free_cmd(&cmd);
    }

    ret = avfilter_graph_config(priv->graph_inst.graph, ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "graph_inst config error %d\n", ret);
        goto fail;
    }

    return 0;
fail:
    asubgraph_close(ctx);
    return ret;
}

static int asubgraph_init(AVFilterContext *ctx)
{
    SubGraphPriv *priv = ctx->priv;
    int i, ret;

    for (i = 0; i < priv->nb_outputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    for (i = 0; i < priv->nb_inputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("input%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        if ((ret = ff_append_inpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    if (priv->map_str) {
        ret = avfilter_parse_mapping(priv->map_str, &priv->map, priv->nb_outputs);
        if (ret < 0)
            return ret;
    }

    SIMPLEQ_INIT(&priv->cmd_queue);

    return 0;
}

static void asubgraph_uninit(AVFilterContext *ctx)
{
    SubGraphPriv *priv = ctx->priv;
    SubCmd *cmd;

    while ((cmd = SIMPLEQ_FIRST(&priv->cmd_queue)) != NULL) {
        SIMPLEQ_REMOVE_HEAD(&priv->cmd_queue, entry);
        asubgraph_free_cmd(&cmd);
    }

    asubgraph_close(ctx);
    av_freep(&priv->map);
    av_freep(&priv->graph_desc);
}

static int asubgraph_try_process_frame(AVFilterContext *ctx, AVFilterLink *inlink,
                                       AVFrame **poframe)
{
    AVFrame *oframe, *iframe = NULL;
    SubGraphPriv *priv = ctx->priv;
    int ret;

    if (!poframe)
        return AVERROR(EINVAL);

    if (!asubgraph_instance_is_inited(&priv->graph_inst)) {
        av_log(ctx, AV_LOG_ERROR, "graph_inst is not initialized.\n");
        return AVERROR(EINVAL);
    }

    oframe = av_frame_alloc();
    if (!oframe)
        return AVERROR(ENOMEM);

    ret = av_buffersink_get_frame(priv->graph_inst.sink_filter, oframe);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        av_log(ctx, AV_LOG_ERROR, "get frame from buffersink error%s\n",
                av_err2str(ret));
        goto err;
    }

    if (ret == AVERROR(EAGAIN)) {
        ret = ff_inlink_consume_frame(inlink, &iframe);
        if (ret < 0)
            goto err;

        if (!iframe) {
            ret = AVERROR(EAGAIN);
            goto err;
        }

        ret = av_buffersrc_add_frame_flags(priv->graph_inst.src_filter, iframe,
                                           AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0)
            goto err;

        ret = av_buffersink_get_frame(priv->graph_inst.sink_filter, oframe);
        if (ret < 0)
            goto err;
    }

    *poframe = oframe;
    av_frame_free(&iframe);
    return 0;

err:
    if (ret != AVERROR(EAGAIN))
        av_log(ctx, AV_LOG_ERROR, "asubgraph process buffer error: %s\n", av_err2str(ret));

    av_frame_free(&oframe);
    av_frame_free(&iframe);
    return ret;
}

static inline int asubgraph_check_link_status_back(AVFilterContext *ctx)
{
    bool need_transfer = true;
    AVFilterLink *outlink;
    int i, ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        outlink = ctx->outputs[i];
        ret = ff_outlink_get_status(outlink);
        if (!ret)
            need_transfer = false;
    }

    if (need_transfer)
        FF_FILTER_FORWARD_STATUS_BACK_ALL(outlink, ctx);

    return 1;
}

static int asubgraph_activate(AVFilterContext *ctx)
{
    AVFrame *iframe = NULL, *oframe = NULL;
    SubGraphPriv *priv = ctx->priv;
    AVFilterLink *inlink, *outlink;
    SubGraphFormats src, sink;
    FilterLinkInternal *li;
    int activate_inpus = 0;
    bool need_close = true;
    bool requested = true;
    int i, pad, status;
    int64_t rpts;
    int ret = 0;

    if (!asubgraph_check_link_status_back(ctx))
        goto out;

    for (i = 0; i < ctx->nb_inputs; i++) {
        inlink = ctx->inputs[i];
        li = ff_link_internal(inlink);

        if (li->status_out)
            continue;

        activate_inpus++;
        pad = i;
    }

    if (activate_inpus == 0)
        return 0;
    else if (activate_inpus > 1) {
        av_log(ctx, AV_LOG_ERROR, "subgraph is not support multi-input.\n");
        return AVERROR_PATCHWELCOME;
    }

    inlink = ctx->inputs[pad];
    ff_inlink_acknowledge_status(inlink, &status, &rpts);
    for (i = 0; i < ctx->nb_outputs; i++) {
        outlink = ctx->outputs[i];

        if ((priv->map && priv->map[i] == ROUTE_OFF) || ff_outlink_get_status(outlink))
            continue;

        if (!asubgraph_instance_is_inited(&priv->graph_inst)) {
            src = (SubGraphFormats) {
                .ch_layout   = inlink->ch_layout,
                .sample_rate = inlink->sample_rate,
                .format      = inlink->format,
            };
            sink = (SubGraphFormats) {
                .ch_layout   = outlink->ch_layout,
                .sample_rate = outlink->sample_rate,
                .format      = outlink->format,
            };

            ret = asubgraph_open(ctx, src, sink);
            if (ret < 0)
                goto out;
        }

        if (!ff_outlink_frame_wanted(outlink) && priv->first_frame_sent)
            requested = false;

        if (!iframe) {
            ret = asubgraph_try_process_frame(ctx, inlink, &iframe);
            if (ret < 0 && ret!= AVERROR(EAGAIN))
                goto out;
        }

        if (iframe) {
            oframe = av_frame_clone(iframe);
            if (!oframe) {
                ret = AVERROR(ENOMEM);
                goto out;
            }

            ret = ff_filter_frame(outlink, oframe);
            if (ret < 0)
                goto out;

            priv->first_frame_sent = true;
            need_close = false;
            ff_filter_set_ready(ctx, 100);
            continue;
        }

        if (status == AVERROR_EOF)
            ff_outlink_set_status(outlink, AVERROR_EOF, AV_NOPTS_VALUE);
        else
            need_close = false;
    }

    if (status >= 0 && requested)
        ff_inlink_request_frame(inlink);

out:
    if (need_close)
        asubgraph_close(ctx);
    av_frame_free(&iframe);
    return ret;
}

static int asubgraph_query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in,
                                   AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *ch_layout = NULL;
    AVFilterFormats *sample_rate = NULL;
    AVFilterFormats *format = NULL;
    SubGraphPriv *priv = ctx->priv;
    FilterLinkInternal *li;
    int activate_pad = -1;
    AVFilterLink *link;
    int ret, i;

    if (!asubgraph_instance_is_inited(&priv->graph_inst))
        return asubgraph_query_alg_formats(ctx, cfg_in, cfg_out);

    for (i = 0; i < ctx->nb_outputs; i++) {
        link = ctx->outputs[i];
        li = ff_link_internal(link);
        if (li->status_in || (priv->map && priv->map[i] == ROUTE_OFF))
            continue;

        activate_pad = i;
        break;
    }

    if (activate_pad == -1)
        return 0;

    if ((ret = ff_add_format(&format, link->format))                < 0 ||
        (ret = ff_add_format(&sample_rate, link->sample_rate))      < 0 ||
        (ret = ff_add_channel_layout(&ch_layout, &link->ch_layout)) < 0 )
        return ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(format, &cfg_out[i]->formats);
        if (ret < 0)
            return ret;

        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(sample_rate, &cfg_out[i]->samplerates);
        if (ret < 0)
            return ret;

        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(ch_layout, &cfg_out[i]->channel_layouts);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int asubgraph_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                     char *res, int res_len, int flags)
{
    SubGraphPriv *priv = ctx->priv;
    int ret, i;

    if (!strcmp(cmd, "dump")) {
        asubgraph_dump(ctx);
        return 0;
    } else if (!strcmp(cmd, "sub_cmd")) {
        return asubgraph_process_subcmd(ctx, args, res, res_len, flags);
    } else if (!strcmp(cmd, "link")) {
        FilterLinkInternal *li;
        for (i = 0; i < ctx->nb_outputs; i++) {
            if (priv->map && priv->map[i] == ROUTE_OFF)
                continue;

            li = ff_link_internal(ctx->outputs[i]);
            li->frame_wanted_out = 1;
        }

        ff_filter_set_ready(ctx, 100);
        return 0;
    } else if (!strcmp(cmd, "map")) {
        int *old_map = NULL;

        if (priv->map) {
            old_map = av_calloc(priv->nb_outputs, sizeof(*old_map));
            if (!old_map)
                return AVERROR(ENOMEM);

            memcpy(old_map, priv->map, priv->nb_outputs * sizeof(*old_map));
        }

        ret = avfilter_parse_mapping(args, &priv->map, priv->nb_outputs);
        if (ret < 0) {
            av_freep(&old_map);
            return ret;
        }

        for (i = 0; i < priv->nb_outputs && old_map; i++) {
            if (old_map[i] != priv->map[i]) {
                if (old_map[i] == ROUTE_ON && priv->map[i] == ROUTE_OFF) {
                    ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
                } else if (old_map[i] == ROUTE_OFF && priv->map[i] == ROUTE_ON) {
                    FilterLinkInternal *li = ff_link_internal(ctx->outputs[i]);
                    li->frame_wanted_out = 1;
                }
            }
        }

        av_freep(&old_map);
        return 0;
    } else if (!strcmp(cmd, "graph_parse")) {
        SubGraphInstance inst = { 0 };
        SubGraphFormats default_fmt = {
            .format = AV_SAMPLE_FMT_S16,
            .sample_rate = 44100,
            .ch_layout = AV_CHANNEL_LAYOUT_STEREO,
        };

        av_freep(&priv->graph_desc);
        priv->graph_desc = av_strdup(args);
        if (!priv->graph_desc)
            return AVERROR(ENOMEM);

        ret = asubgraph_init_instance(ctx, &inst, &default_fmt, NULL);
        if (ret < 0)
            return ret;

        asubgraph_uninit_instance(&inst);
        return 0;
    } else {
        av_log(ctx, AV_LOG_ERROR, "%s unknown command: %s\n", ctx->name, cmd);
        return AVERROR(EINVAL);
    }
}

#define OFFSET(x) offsetof(SubGraphPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption asubgraph_options[] = {
    { "inputs",            "", OFFSET(nb_inputs),         AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "outputs",           "", OFFSET(nb_outputs),        AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "map",               "", OFFSET(map_str),           AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "map_array",         "", OFFSET(map),               AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { NULL },
};

static const AVClass asubgraph_class = {
    .class_name          = "asubgraph_class",
    .item_name           = av_default_item_name,
    .option              = asubgraph_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_af_asubgraph = {
    .name            = "asubgraph",
    .description     = NULL_IF_CONFIG_SMALL("Audio subgraph"),
    .priv_class      = &asubgraph_class,
    .priv_size       = sizeof(SubGraphPriv),
    .init            = asubgraph_init,
    .uninit          = asubgraph_uninit,
    FILTER_QUERY_FUNC2(asubgraph_query_formats),
    .activate        = asubgraph_activate,
    .process_command = asubgraph_process_command,
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};