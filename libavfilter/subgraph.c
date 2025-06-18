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

#include "avfilter_internal.h"
#include "buffersink.h"
#include "buffersrc.h"
#include "subgraph.h"
#include "formats.h"

static int subgraph_create_graph(AVSubGraphContext *ctx,
                                 const char *graph_desc,
                                 int in_sample_rate, int out_sample_rate,
                                 enum AVSampleFormat in_format,
                                 enum AVSampleFormat out_format,
                                 AVChannelLayout in_ch_layout,
                                 AVChannelLayout out_ch_layout)
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

    // create src filter
    ret = av_channel_layout_describe(&in_ch_layout, channel_layout_str,
                                     sizeof(channel_layout_str));
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "failed to describe input channel layout\n");
        goto fail;
    }

    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             in_sample_rate, av_get_sample_fmt_name(in_format),
             channel_layout_str);

    ret = avfilter_graph_create_filter(&src_filter, avfilter_get_by_name("abuffer"),
                                       "abuffer", args, NULL, graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot create audio source filter ret %d.\n", ret);
        goto fail;
    }

    // create sink filter
    memset(args, 0, sizeof(args));
    memset(channel_layout_str, 0, sizeof(channel_layout_str));
    ret = av_channel_layout_describe(&out_ch_layout, channel_layout_str,
                                     sizeof(channel_layout_str));
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "failed to describe output channel layout\n");
        goto fail;
    }

    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             out_sample_rate, av_get_sample_fmt_name(out_format),
             channel_layout_str);

    ret = avfilter_graph_create_filter(&sink_filter, avfilter_get_by_name("abuffersink"),
                                       "abuffersink", NULL, NULL, graph);
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

static int subgraph_copy_formats(AVSubGraphFormats *dest, AVFilterFormatsConfig *src)
{
    int ret = AVERROR(ENOMEM);
    int i;

    if (!src || !dest)
        return AVERROR(EINVAL);

    if (src->samplerates &&
        (dest->nb_sample_rates = src->samplerates->nb_formats) > 0) {
        dest->sample_rates = av_mallocz(dest->nb_sample_rates * sizeof(*dest->sample_rates));
        if (!dest->sample_rates)
            goto fail;

        for (i = 0; i < dest->nb_sample_rates; i++)
            dest->sample_rates[i] = src->samplerates->formats[i];
    }

    if (src->formats &&
        (dest->nb_formats = src->formats->nb_formats) > 0) {
        dest->formats = av_mallocz(dest->nb_formats * sizeof(*dest->formats));
        if (!dest->formats)
            goto fail;

        for (i = 0; i < dest->nb_formats; i++)
            dest->formats[i] = src->formats->formats[i];
    }

    if (src->channel_layouts &&
        (dest->nb_channel_layouts = src->channel_layouts->nb_channel_layouts) > 0) {
        dest->channel_layouts = av_mallocz(dest->nb_channel_layouts * sizeof(*dest->channel_layouts));
        if (!dest->channel_layouts)
            goto fail;

        for (i = 0; i < dest->nb_channel_layouts; i++) {
            if (ret = av_channel_layout_copy(&dest->channel_layouts[i],
                                             &src->channel_layouts->channel_layouts[i]) < 0)
                goto fail;
        }
    }

    return 0;

fail:
    if (dest->sample_rates)    av_free(dest->sample_rates);
    if (dest->formats)         av_free(dest->formats);
    if (dest->channel_layouts) av_free(dest->channel_layouts);
    memset(dest, 0, sizeof(*dest));
    return ret;
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

void avfilter_asubgraph_free_formats(AVSubGraphFormats **cfgp)
{
    AVSubGraphFormats *cfg;
    if (!cfgp || !*cfgp)
        return;

    cfg = *cfgp;
    if (cfg->sample_rates)
        av_free(cfg->sample_rates);
    if (cfg->formats)
        av_free(cfg->formats);
    if (cfg->channel_layouts)
        av_free(cfg->channel_layouts);

    av_freep(cfgp);
}

int avfilter_asubgraph_query_formats(const char *graph_desc, AVSubGraphFormats **cfg_in,
                                     AVSubGraphFormats **cfg_out)
{
    AVFilterFormatsConfig *graph_cfg_in, *graph_cfg_out;
    AVSubGraphContext *graph;
    AVFilterContext *dest_filter;
    AVChannelLayout ch_layout;
    int64_t period_time = 0;
    char args[64];
    int ret;
    int i;

    if (!graph_desc || !*graph_desc) {
        av_log(NULL, AV_LOG_ERROR, "without graph_desc, can not query fromats.\n");
        return AVERROR(EINVAL);
    }

    av_log(NULL, AV_LOG_INFO, "subgraph query_formats: %s.\n", graph_desc);

    graph = av_mallocz(sizeof(AVSubGraphContext));
    if (!graph)
        return AVERROR(ENOMEM);

    //Set tmp fmt without actually using it
    av_channel_layout_default(&ch_layout, 1);
    ret = subgraph_create_graph(graph, graph_desc, 16000, 16000,
                                AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16,
                                ch_layout, ch_layout);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot create subgraph %d.\n", ret);
        goto fail;
    }

    for (i = 0; i < graph->graph->nb_filters; i++) {
        if (graph->graph->filters[i] != graph->src_filter &&
            graph->graph->filters[i] != graph->sink_filter &&
            strncmp(graph->graph->filters[i]->name, "auto", 4) != 0) {
            dest_filter = graph->graph->filters[i];
            if (dest_filter->filter->formats_state != FF_FILTER_FORMATS_QUERY_FUNC2) {
                av_log(NULL, AV_LOG_ERROR, "filter %s not support query_formats.\n",
                       dest_filter->name);
                ret = AVERROR(EINVAL);
                goto fail;
            }

            graph_cfg_in  = &dest_filter->inputs[0]->outcfg;
            graph_cfg_out = &dest_filter->outputs[0]->incfg;
            dest_filter->filter->formats.query_func2(dest_filter, &graph_cfg_in,
                                                     &graph_cfg_out);
            break;
        }
    }

    *cfg_in = av_mallocz(sizeof(**cfg_in));
    *cfg_out = av_mallocz(sizeof(**cfg_in));
    if (!*cfg_in || !*cfg_out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = subgraph_copy_formats(*cfg_in, graph_cfg_in);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "copy input formats error %d\n", ret);
        goto fail;
    }

    ret = subgraph_copy_formats(*cfg_out, graph_cfg_out);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "copy output formats error %d\n", ret);
        goto fail;
    }

    if (!av_opt_get_int(dest_filter->priv, "period_time", 0, &period_time)) {
        (*cfg_in)->period_time  = (int)period_time;
        (*cfg_out)->period_time = (int)period_time;
        av_log(NULL, AV_LOG_INFO, "subgraph need period_time = %d\n", (int)period_time);
    }

    avfilter_asubgraph_uninit(&graph);
    return 0;

fail:
    avfilter_asubgraph_free_formats(cfg_in);
    avfilter_asubgraph_free_formats(cfg_out);
    avfilter_asubgraph_uninit(&graph);
    return ret;
}

int avfilter_asubgraph_init(AVSubGraphContext **ctxp,
                            const char *graph_desc,
                            int in_sample_rate,
                            int out_sample_rate,
                            enum AVSampleFormat in_format,
                            enum AVSampleFormat out_format,
                            AVChannelLayout in_ch_layout,
                            AVChannelLayout out_ch_layout)
{
    AVSubGraphContext *ctx = *ctxp;
    AVSubCmd *cmd;
    int ret;

    if (!graph_desc || !*graph_desc) {
        av_log(NULL, AV_LOG_ERROR, "graph_desc is needed.\n");
        return AVERROR(EINVAL);
    }

    if (!ctx && !(*ctxp = ctx = av_mallocz(sizeof(*ctx))))
        return AVERROR(ENOMEM);

    av_log(NULL, AV_LOG_INFO, "subgraph init parms: %s %d %d %d %d %d %d.\n",
           graph_desc, in_sample_rate, out_sample_rate,
           in_format, out_format, in_ch_layout.nb_channels,
           out_ch_layout.nb_channels);

    // all format should be setted as vaild value
    if (out_ch_layout.nb_channels <= 0 || in_ch_layout.nb_channels <= 0 ||
        in_sample_rate <= 0 || out_sample_rate <= 0 ||
        in_format < 0 || out_format < 0) {
        av_log(NULL, AV_LOG_ERROR, "invalid parameters.\n");
        return AVERROR(EINVAL);
    }

    ret = subgraph_create_graph(ctx, graph_desc, in_sample_rate, out_sample_rate,
                                in_format, out_format,
                                in_ch_layout, out_ch_layout);
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
    avfilter_asubgraph_uninit(&ctx);
    return ret;
}

void avfilter_asubgraph_uninit(AVSubGraphContext **ctxp)
{
    if (!ctxp || !*ctxp)
        return;

    if ((*ctxp)->graph)
        avfilter_graph_free(&(*ctxp)->graph);

    if ((*ctxp)->cmd_queue) {
        subgraph_clear_cmdq((*ctxp)->cmd_queue);
        av_free((*ctxp)->cmd_queue);
    }

    av_freep(ctxp);
}

int avfilter_asubgraph_process(AVSubGraphContext *ctx, AVFrame *frame)
{
    int ret;

    if (!subgraph_is_inited(ctx)) {
        av_log(NULL, AV_LOG_ERROR, "subgraph is not initialized.\n");
        return AVERROR(EINVAL);
    }

    if ((ret = av_buffersrc_add_frame_flags(ctx->src_filter, frame,
                                            AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "submitt frame to buffersrc error %s\n",
               av_err2str(ret));
        return ret;
    }

    av_frame_unref(frame);

    if ((ret = av_buffersink_get_frame(ctx->sink_filter, frame)) < 0) {
        if (ret != AVERROR(EAGAIN))
            av_log(NULL, AV_LOG_ERROR, "get frame from buffersink error%s\n",
                   av_err2str(ret));
        av_free(frame);
        return ret;
    }

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