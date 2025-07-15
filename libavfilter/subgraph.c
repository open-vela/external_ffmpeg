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
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/avstring.h>

#include "avfilter_internal.h"
#include "buffersink.h"
#include "buffersrc.h"
#include "subgraph.h"
#include "formats.h"

static int subgraph_create_graph(AVSubGraphContext *ctx,
                                 const char *graph_desc,
                                 const AVAudioFormats *src_fmt,
                                 const AVAudioFormats *sink_fmt)
{
    AVFilterInOut *outputs = NULL, *inputs = NULL;
    AVFilterContext *src_filter, *sink_filter;
    char channel_layout_str[16];
    AVFilterGraph *graph;
    char args[64];
    int ret;
    int i;

    graph = avfilter_graph_alloc();
    if (!graph)
        return AVERROR(ENOMEM);

    ret = av_channel_layout_describe(&src_fmt->ch_layout, channel_layout_str,
                                     sizeof(channel_layout_str));
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "failed to describe src_fmt channel layout\n");
        goto fail;
    }

    snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             src_fmt->sample_rate, av_get_sample_fmt_name(src_fmt->format), channel_layout_str);

    ret = avfilter_graph_create_filter(&src_filter, avfilter_get_by_name("abuffer"),
                                       "abuffer", args, NULL, graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot create audio source filter ret %d.\n", ret);
        goto fail;
    }

    // create sink filter
    if (sink_fmt) {
        memset(args, 0, sizeof(args));
        memset(channel_layout_str, 0, sizeof(channel_layout_str));
        ret = av_channel_layout_describe(&sink_fmt->ch_layout, channel_layout_str,
                                        sizeof(channel_layout_str));
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "failed to describe sink_fmt channel layout\n");
            goto fail;
        }
        snprintf(args, sizeof(args), "samplerates=%d:sample_formats=%s:channel_layouts=%s",
                 sink_fmt->sample_rate, av_get_sample_fmt_name(sink_fmt->format), channel_layout_str);
    }

    ret = avfilter_graph_create_filter(&sink_filter, avfilter_get_by_name("abuffersink"),
                                       "abuffersink", sink_fmt ? args : NULL, NULL, graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot create audio sink filter ret %d.\n", ret);
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
    ret = avfilter_graph_parse_ptr(graph, graph_desc, &inputs, &outputs, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "parsing subgraph error %d\n", ret);
        goto fail;
    }

    ctx->graph       = graph;
    ctx->src_filter  = src_filter;
    ctx->sink_filter = sink_filter;
    return 0;

fail:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&graph);
    return ret;
}

static inline int subgraph_is_inited(AVSubGraphContext *ctx)
{
    return (ctx && ctx->graph);
}

static void subgraph_free_cmd(AVSubCmd **cmd)
{
    if (!cmd || !*cmd)
        return;

    if ((*cmd)->cmd)  av_free((*cmd)->cmd);
    av_freep(cmd);
}

static void subgraph_clear_cmdq(struct AVSubCmdQueue *cmd_queue)
{
    AVSubCmd *cmd;

    while ((cmd = SIMPLEQ_FIRST(cmd_queue)) != NULL) {
        SIMPLEQ_REMOVE_HEAD(cmd_queue, entry);
        subgraph_free_cmd(&cmd);
    }
}

static struct AVSubCmdQueue *subgraph_init_cmdq(void)
{
    struct AVSubCmdQueue *ret = av_mallocz(sizeof(*ret));

    if (!ret)
        return NULL;

    SIMPLEQ_INIT(ret);
    return ret;
}

static int subgraph_enqueue_cmd(struct AVSubCmdQueue *cmd_queue, const char *cmd)
{
    AVSubCmd *new_cmd;

    new_cmd = av_mallocz(sizeof(AVSubCmd));
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

static int subgraph_dump(AVSubGraphContext *ctx)
{
    char* dump;

    if (!subgraph_is_inited(ctx))
        return AVERROR(EINVAL);

    dump = avfilter_graph_dump(ctx->graph, NULL);
    if (dump != NULL) {
        av_log(NULL, AV_LOG_INFO, "subgraph dump:\n%s\n", dump);
        av_free(dump);
    } else {
        av_log(NULL, AV_LOG_ERROR, "unable to dump subgraph\n");
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int subgraph_process_cmd(AVSubGraphContext *ctx, const char *args,
                                char *res, int res_len, int flags)
{
    char *p, *target, *sub_cmd, *sub_args;
    char *saveptr = NULL;
    int ret;
    int i;

    if (!args)
        return AVERROR(EINVAL);

    // when subgraph is not initialized, enqueue the cmd to cmd_queue
    if (!subgraph_is_inited(ctx)) {
        if (!ctx)
            return AVERROR(EINVAL);

        if (!ctx->cmd_queue && !(ctx->cmd_queue = subgraph_init_cmdq()))
            return AVERROR(ENOMEM);

        av_log(NULL, AV_LOG_INFO, "subgraph is not init, pending sub_cmd: %s\n", args);
        ret = subgraph_enqueue_cmd(ctx->cmd_queue, args);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "enqueue_cmd error %d\n", ret);
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
        av_log(NULL, AV_LOG_ERROR, "invalid format for sub_cmd: %s\n", args);
        ret = AVERROR(EINVAL);
        goto end;
    }

    av_log(NULL, AV_LOG_INFO, "process sub_cmd: %s %s %s.\n", target, sub_cmd, sub_args);

    for (i = 0; i < ctx->graph->nb_filters; i++) {
        AVFilterContext *filter = ctx->graph->filters[i];
        if ((filter->name && !strcmp(target, filter->name))
            || !strcmp(target, filter->filter->name)) {
            ret = avfilter_process_command(filter, sub_cmd, sub_args, res, res_len, flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "error executing filter(%s) command %s %s %d\n",
                       target, sub_cmd, args, ret);
            }
            goto end;
        }
    }

    av_log(NULL, AV_LOG_ERROR, "filter %s not found\n", target);
    ret = AVERROR(ENOENT);

end:
    av_freep(&p);
    return ret;
}

void avfilter_asubgraph_reinit_formats(AVAudioFormats *cfg)
{
    av_channel_layout_uninit(&cfg->ch_layout);
    cfg->format = AV_SAMPLE_FMT_NONE;
    cfg->sample_rate = 0;
}

int avfilter_asubgraph_query_formats(const char *graph_desc, const AVAudioFormats *sug_fmt,
                                     AVAudioFormats *src_fmt, AVAudioFormats *sink_fmt)
{
    const AVAudioFormats *sug_src_fmt, *sug_sink_fmt;
    AVFilterLink *in_link = NULL, *out_link = NULL;
    AVFilterContext *dest_filter = NULL;
    AVSubGraphContext subgraph = { 0 };
    AVAudioFormats default_fmt;
    char *dest_name;
    char tmp[64];
    int ret;
    int i;

    if (!graph_desc || !*graph_desc) {
        av_log(NULL, AV_LOG_ERROR, "without graph_desc, can not query fromats.\n");
        return AVERROR(EINVAL);
    }

    av_log(NULL, AV_LOG_INFO, "subgraph query_formats: %s.\n", graph_desc);

    if (sug_fmt) {
        if (sug_fmt->sample_rate <= 0 || sug_fmt->format == AV_SAMPLE_FMT_NONE
            || sug_fmt->ch_layout.nb_channels <= 0) {
            av_log(NULL, AV_LOG_ERROR, "invalid sug_fmt.\n");
            return AVERROR(EINVAL);
        }

        sug_src_fmt  = sug_fmt;
        sug_sink_fmt = sug_fmt;
    } else {
        default_fmt.sample_rate = 8000;
        default_fmt.format = AV_SAMPLE_FMT_S16;
        av_channel_layout_default(&default_fmt.ch_layout, 1);
        sug_src_fmt  = &default_fmt;
        sug_sink_fmt = NULL;
    }

    ret = subgraph_create_graph(&subgraph, graph_desc, sug_src_fmt, sug_sink_fmt);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot create subgraph %d.\n", ret);
        goto fail;
    }

    ret = avfilter_graph_config(subgraph.graph, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "subgraph config error %d\n", ret);
        goto fail;
    }

    av_strlcpy(tmp, graph_desc, sizeof(tmp));
    dest_name = strtok_r(tmp, "=", NULL);
    if (!dest_name) {
        av_log(NULL, AV_LOG_ERROR, "find dest_filter failed.\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    for (i = 0; i < subgraph.graph->nb_filters; i++) {
        AVFilterContext *filter = subgraph.graph->filters[i];
        if ((filter->name && !strcmp(dest_name, filter->name))
            || !strcmp(dest_name, filter->filter->name)) {
            dest_filter = subgraph.graph->filters[i];
            in_link = dest_filter->inputs[0];
            out_link = dest_filter->outputs[0];
            break;
        }
    }

    if (!in_link || !out_link) {
        av_log(NULL, AV_LOG_ERROR, "illegal in_link or out_link.\n");
        ret = AVERROR(EINVAL);
        goto fail;
    }

    if (src_fmt) {
        src_fmt->sample_rate = in_link->sample_rate;
        src_fmt->format = in_link->format;
        ret = av_channel_layout_copy(&src_fmt->ch_layout, &in_link->ch_layout);
        if (ret < 0)
            goto fail;
    }

    if (sink_fmt) {
        sink_fmt->sample_rate = out_link->sample_rate;
        sink_fmt->format = out_link->format;
        ret = av_channel_layout_copy(&sink_fmt->ch_layout, &out_link->ch_layout);
        if (ret < 0)
            goto fail;
    }

    avfilter_asubgraph_uninit(&subgraph);
    return 0;

fail:
    avfilter_asubgraph_uninit(&subgraph);
    if (src_fmt)  avfilter_asubgraph_reinit_formats(src_fmt);
    if (sink_fmt) avfilter_asubgraph_reinit_formats(sink_fmt);
    return ret;
}

int avfilter_asubgraph_init(AVSubGraphContext *ctx,
                            const char *graph_desc,
                            const AVAudioFormats src_fmt,
                            const AVAudioFormats sink_fmt)
{
    AVSubCmd *cmd;
    int ret;

    if (!graph_desc || !*graph_desc) {
        av_log(NULL, AV_LOG_ERROR, "graph_desc is needed.\n");
        return AVERROR(EINVAL);
    }

    av_log(NULL, AV_LOG_INFO, "subgraph init parms: %s %d %d %d(src) %d %d %d(sink).\n",
           graph_desc, src_fmt.format, src_fmt.sample_rate, src_fmt.ch_layout.nb_channels,
           sink_fmt.format, sink_fmt.sample_rate, sink_fmt.ch_layout.nb_channels);

    // all format should be setted as vaild value
    if (src_fmt.ch_layout.nb_channels <= 0   || sink_fmt.ch_layout.nb_channels <= 0 ||
        src_fmt.sample_rate <= 0             || sink_fmt.sample_rate          <= 0  ||
        src_fmt.format <= AV_SAMPLE_FMT_NONE || sink_fmt.format <= AV_SAMPLE_FMT_NONE) {
        av_log(NULL, AV_LOG_ERROR, "invalid parameters.\n");
        return AVERROR(EINVAL);
    }

    ret = subgraph_create_graph(ctx, graph_desc, &src_fmt, &sink_fmt);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot create audio subgraph %d.\n", ret);
        goto fail;
    }

    while (ctx->cmd_queue && (cmd = SIMPLEQ_FIRST(ctx->cmd_queue)) != NULL) {
        ret = subgraph_process_cmd(ctx, cmd->cmd, NULL, 0, 0);
        if (ret < 0)
            av_log(NULL, AV_LOG_ERROR, "error processing sub_cmd: %s, ret=%d\n", cmd->cmd, ret);

        SIMPLEQ_REMOVE_HEAD(ctx->cmd_queue, entry);
        subgraph_free_cmd(&cmd);
    }

    ret = avfilter_graph_config(ctx->graph, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "subgraph config error %d\n", ret);
        goto fail;
    }

    return 0;
fail:
    avfilter_asubgraph_uninit(ctx);
    return ret;
}

void avfilter_asubgraph_uninit(AVSubGraphContext *ctx)
{
    if (!ctx)
        return;

    if (ctx->graph)
        avfilter_graph_free(&ctx->graph);

    if (ctx->cmd_queue) {
        subgraph_clear_cmdq(ctx->cmd_queue);
        av_freep(ctx->cmd_queue);
    }
}

int avfilter_asubgraph_process(AVSubGraphContext *ctx, AVFrame *iframe, AVFrame **poframe)
{
    AVFrame *oframe;
    int ret;

    if (!poframe)
        return AVERROR(EINVAL);

    if (!subgraph_is_inited(ctx)) {
        av_log(NULL, AV_LOG_ERROR, "subgraph is not initialized.\n");
        return AVERROR(EINVAL);
    }

    if (iframe) {
        ret = av_buffersrc_add_frame_flags(ctx->src_filter, iframe, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "submitt frame to buffersrc error %s\n",
                av_err2str(ret));
            return ret;
        }
    }

    oframe = av_frame_alloc();
    if (!oframe)
        return AVERROR(ENOMEM);

    if ((ret = av_buffersink_get_frame(ctx->sink_filter, oframe)) < 0) {
        if (ret != AVERROR(EAGAIN))
            av_log(NULL, AV_LOG_ERROR, "get frame from buffersink error%s\n",
                   av_err2str(ret));
        av_frame_free(&oframe);
        return ret;
    }

    *poframe = oframe;
    return 0;
}

int avfilter_asubgraph_process_command(AVSubGraphContext *ctx, const char *cmd, const char *args,
                                       char *res, int res_len, int flags)
{
    if (!strcmp(cmd, "dump")) {
        subgraph_dump(ctx);
        return 0;
    } else if (!strcmp(cmd, "sub_cmd")) {
        return subgraph_process_cmd(ctx, args, res, res_len, flags);
    } else {
        av_log(NULL, AV_LOG_ERROR, "unknown command for subgraph %s\n", cmd);
        return AVERROR(EINVAL);
    }
}