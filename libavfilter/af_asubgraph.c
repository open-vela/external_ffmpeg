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
#include "amix.h"

#include <sys/queue.h>

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

/**
 * @struct SubGraphFormats
 * Configuration for supported audio formats in subgraph
 */
typedef struct SubGraphFormats {
    int sample_rate;
    enum AVSampleFormat format;
    AVChannelLayout ch_layout;
} SubGraphFormats;

typedef struct SubGraphPriv {
    const AVClass *class;

    SubGraphInstance graph_inst;
    struct SubCmdQueue cmd_queue;

    int nb_inputs;
    int nb_outputs;
    char *map_str;
    int *map;

    char *graph_desc;
    AMixContext *mix;
    SubGraphFormats *mix_fmts;
    SubGraphFormats *out_fmts;
    int64_t pts;

    bool drained;
    bool first_frame_sent;
} SubGraphPriv;

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
    av_log(logger, AV_LOG_INFO, "%s init inst success\n", ctx->name);
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

    av_freep(&priv->mix_fmts);
    av_freep(&priv->out_fmts);
    if (priv->mix) {
        ff_amix_free(priv->mix);
        priv->mix = NULL;
    }

    priv->pts              = AV_NOPTS_VALUE;
    priv->first_frame_sent = false;
    asubgraph_uninit_instance(&priv->graph_inst);
    av_log(ctx, AV_LOG_INFO, "%s close.\n", ctx->name);
}

static int asubgraph_find_avaiable_fmts(const AVFilterContext *ctx)
{
    SubGraphPriv *priv = ctx->priv;
    AVFilterLink *link;
    int i, ret = AVERROR(ENOMEM);

    for (i = 0; i < ctx->nb_inputs ; i++) {
        link = ctx->inputs[i];
        if ((ff_link_internal(link)->status_out) != AVERROR_EOF      &&
            link->sample_rate > 0 && link->ch_layout.nb_channels > 0 &&
            link->format > AV_SAMPLE_FMT_NONE && link->format < AV_SAMPLE_FMT_NB) {
            if (!priv->mix_fmts && !(priv->mix_fmts = av_mallocz(sizeof(*priv->mix_fmts))))
                goto err;
            priv->mix_fmts->ch_layout   = link->ch_layout;
            priv->mix_fmts->sample_rate = link->sample_rate;
            priv->mix_fmts->format      = link->format;
            break;
        }
    }

    for (i = 0; i < ctx->nb_outputs ; i++) {
        link =  ctx->outputs[i];
        if ((ff_link_internal(link)->status_in) != AVERROR_EOF       &&
            priv->map && priv->map[i] != ROUTE_OFF                   &&
            link->sample_rate > 0 && link->ch_layout.nb_channels > 0 &&
            link->format > AV_SAMPLE_FMT_NONE && link->format < AV_SAMPLE_FMT_NB) {
            if (!priv->out_fmts && !(priv->out_fmts = av_mallocz(sizeof(*priv->out_fmts))))
                goto err;
            priv->out_fmts->ch_layout   = link->ch_layout;
            priv->out_fmts->sample_rate = link->sample_rate;
            priv->out_fmts->format      = link->format;
            break;
        }
    }

    if (!priv->mix_fmts || !priv->out_fmts) {
        av_log((void *)ctx, AV_LOG_ERROR, "%s cannot find %s/%s format.\n", ctx->name,
               priv->mix_fmts ? "input" : "", priv->out_fmts ? "output" : "");
        ret = AVERROR(EINVAL);
        goto err;
    }

    return 0;
err:
    av_freep(&priv->mix_fmts);
    av_freep(&priv->out_fmts);
    return ret;
}

static int asubgraph_open(AVFilterContext *ctx)
{
    SubGraphPriv *priv = ctx->priv;
    SubCmd *cmd;
    int ret;

    if (asubgraph_instance_is_inited(&priv->graph_inst))
        return 0;

    if (!priv->mix_fmts || !priv->out_fmts) {
        ret = asubgraph_find_avaiable_fmts(ctx);
        if (ret < 0)
            return ret;
    }

    priv->mix = ff_amix_alloc(priv->mix_fmts->sample_rate, priv->mix_fmts->format, 
                              priv->mix_fmts->ch_layout.nb_channels);
    if (!priv->mix) {
        av_log(ctx, AV_LOG_ERROR, "cannot allocate amix.\n");
        return AVERROR(ENOMEM);
    }

    av_log(ctx, AV_LOG_INFO, "%s graph_inst init parms:\n",ctx->name);
    av_log(ctx, AV_LOG_INFO, "src: format=%s sample_rate=%d ch_layout=%d.\n",
           av_get_sample_fmt_name(priv->mix_fmts->format), priv->mix_fmts->sample_rate,
           priv->mix_fmts->ch_layout.nb_channels);
    av_log(ctx, AV_LOG_INFO, "sink: format=%s sample_rate=%d ch_layout=%d.\n",
           av_get_sample_fmt_name(priv->out_fmts->format), priv->out_fmts->sample_rate,
           priv->out_fmts->ch_layout.nb_channels);

    ret = asubgraph_init_instance(ctx, &priv->graph_inst, priv->mix_fmts, priv->out_fmts);
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

    priv->pts              = 0;
    priv->drained          = false;
    priv->first_frame_sent = false;
    av_log(ctx, AV_LOG_INFO, "%s open success.\n", ctx->name);
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

static int asubgraph_drain_mix(AVFilterContext *ctx)
{
    SubGraphPriv *priv = ctx->priv;
    AVFrame *frame;
    int ret;

    for (;;) {
        ret = ff_amix_read(priv->mix, &frame);
        if (ret <= 0)
            break;

        ret = av_buffersrc_add_frame_flags(priv->graph_inst.src_filter, frame, 0);
        av_frame_free(&frame);
        if (ret < 0)
            break;
    }

    priv->drained = true;
    if (ret = AVERROR(EAGAIN))
        ret = 0;
    av_log(ctx, AV_LOG_INFO, "%s: drain mix %d\n", ctx->name, ret);
    for (int i = 0; i < priv->nb_inputs; i++)
        av_log(ctx, AV_LOG_DEBUG, "mix input%d:%d\n", i,
               ff_amix_input_empty(priv->mix, ctx->inputs[i]));
    return ret;
}

static int asubgraph_try_process_frame(AVFilterContext *ctx, AVFrame **poframe)
{
    AVFrame *oframe, *iframe = NULL;
    SubGraphPriv *priv = ctx->priv;
    int ret;

    if (!poframe)
        return AVERROR(EINVAL);

    oframe = av_frame_alloc();
    if (!oframe)
        return AVERROR(ENOMEM);

    ret = av_buffersink_get_frame(priv->graph_inst.sink_filter, oframe);
    if (ret < 0 && ret != AVERROR(EAGAIN))
        goto err;

    if (ret == AVERROR(EAGAIN)) {
        ret = ff_amix_read(priv->mix, &iframe);
        if (ret <= 0) {
            if (ret == 0)
                ret = AVERROR(EAGAIN);
            goto err;
        }

        ret = av_buffersrc_add_frame_flags(priv->graph_inst.src_filter, iframe, 0);
        av_frame_free(&iframe);
        if (ret < 0)
            goto err;

        ret = av_buffersink_get_frame(priv->graph_inst.sink_filter, oframe);
        if (ret < 0)
            goto err;
    }

    *poframe = oframe;
    return 0;

err:
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        av_log(ctx, AV_LOG_ERROR, "%s: asubgraph process buffer error: %s\n",
               ctx->name, av_err2str(ret));
    av_frame_free(&oframe);
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

    // If transferred EOF, the func returns 0.
    if (need_transfer)
        FF_FILTER_FORWARD_STATUS_BACK_ALL(outlink, ctx);

    return 1;
}

static int asubgraph_output_frame(AVFilterContext *ctx, bool need_drain)
{
    AVFrame *oframe, *iframe = NULL;
    SubGraphPriv *priv = ctx->priv;
    int ret = 0;
    int i;

    if (need_drain && !priv->drained) {
        asubgraph_drain_mix(ctx);
        ret = av_buffersrc_add_frame_flags(priv->graph_inst.src_filter, NULL, 0);
        av_log(ctx, AV_LOG_INFO, "%s: EOF is reached, send the eos to graph_inst.\n",
               ctx->name);
    }

    ret = asubgraph_try_process_frame(ctx, &iframe);
    if (ret < 0)
        return ret;

    iframe->pts = priv->pts;
    iframe->duration = av_rescale_q(iframe->nb_samples, av_make_q(1, iframe->sample_rate), AV_TIME_BASE_Q);
    priv->pts += iframe->duration;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if ((priv->map && priv->map[i] == ROUTE_OFF) || ff_outlink_get_status(ctx->outputs[i]))
            continue;

        oframe = av_frame_clone(iframe);
        if (!oframe) {
            ret = AVERROR(ENOMEM);
            break;
        }

        ret = ff_filter_frame(ctx->outputs[i], oframe);
        if (ret < 0)
            break;

        priv->first_frame_sent = true;
        ff_filter_set_ready(ctx, 100);
    }

    av_frame_free(&iframe);
    return ret;
}

static int asubgraph_activate(AVFilterContext *ctx)
{
    SubGraphPriv *priv = ctx->priv;
    int64_t rpts = AV_NOPTS_VALUE;
    bool eof_forward = true;
    int ret = AVERROR_EOF;
    bool request = true;
    int status;
    int i;

    if (!asubgraph_check_link_status_back(ctx))
        goto out;

    for (i = 0; i < ctx->nb_inputs; i++) {
        if ((ff_link_internal(ctx->inputs[i])->status_out) == AVERROR_EOF)
            continue;

        ret = asubgraph_open(ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "%s: asubgraph open failed, ret:%d.\n",
                   ctx->name, ret);
            goto out;
        }

        if (ff_inlink_check_available_frame(ctx->inputs[i])) {
            ret = ff_amix_input_write(priv->mix, ctx->inputs[i]);
            if (ret < 0)
                av_log(ctx, AV_LOG_ERROR, "%s input[%d] write to mix failed, ret:%d.\n",
                       ctx->name, i, ret);
        }

        // Transfer eof_forward after all data on inlink be written to amix.
        if (!ff_outlink_get_status(ctx->inputs[i]) || !ff_amix_input_write_down(priv->mix, ctx->inputs[i]))
            eof_forward = false;
    }

    // check unexpected activate.
    if (!asubgraph_instance_is_inited(&priv->graph_inst)) {
        av_log(ctx, AV_LOG_WARNING, "WARN: %s NOT init, accidentally activated.\n", ctx->name);
        goto status_check;
    }

    // When all frames in amix & priv->graph_inst are passed, the ret is AVERROR_EOF.
    ret = asubgraph_output_frame(ctx, eof_forward);
status_check:
    for (i = 0; i < ctx->nb_outputs; i++) {
        if ((priv->map && priv->map[i] == ROUTE_OFF) || ff_outlink_get_status(ctx->outputs[i]))
            continue;

        if (ret == AVERROR_EOF) {
            ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, rpts);
            request = false;
            continue;
        }

        if (!ff_outlink_frame_wanted(ctx->outputs[i]) && priv->first_frame_sent)
            request = false;
    }

    for (i = 0; i < ctx->nb_inputs; i++) {
        // When eof_forward=true, the inlink states should be synchronized after all frames have been passed.
        if (!eof_forward || ret == AVERROR_EOF)
            ff_inlink_acknowledge_status(ctx->inputs[i], &status, &rpts);
        if (request && ff_amix_input_want(priv->mix, ctx->inputs[i]) && !ff_outlink_get_status(ctx->inputs[i]))
            ff_inlink_request_frame(ctx->inputs[i]);
    }

out:
    if (ret == AVERROR_EOF)
        asubgraph_close(ctx);
    return ret;
}

static int asubgraph_query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in,
                                   AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *in_ch_layout = NULL, *out_ch_layout = NULL;
    AVFilterFormats *in_sample_rate = NULL, *out_sample_rate = NULL;
    AVFilterFormats *in_format = NULL, *out_format = NULL;
    SubGraphPriv *priv = ctx->priv;
    int ret, i;

    if (!priv->mix_fmts || !priv->out_fmts) {
        if (asubgraph_instance_is_inited(&priv->graph_inst))
            return AVERROR(EINVAL);

        ret = asubgraph_find_avaiable_fmts(ctx);
        if (ret < 0)
            return asubgraph_query_alg_formats(ctx, cfg_in, cfg_out);
    }

    // Copy the mix format to each in_link
    for (i = 0; i < ctx->nb_inputs; i++) {
        ret = ff_add_format(&in_format, priv->mix_fmts->format);
        if (ret < 0)
            return ret;

        ff_formats_unref(&cfg_in[i]->formats);
        ret = ff_formats_ref(in_format, &cfg_in[i]->formats);
        if (ret < 0)
            return ret;

        ret = ff_add_format(&in_sample_rate, priv->mix_fmts->sample_rate);
        if (ret < 0)
            return ret;

        ff_formats_unref(&cfg_in[i]->samplerates);
        ret = ff_formats_ref(in_sample_rate, &cfg_in[i]->samplerates);
        if (ret < 0)
            return ret;

        ret = ff_add_channel_layout(&in_ch_layout, &priv->mix_fmts->ch_layout);
        if (ret < 0)
            return ret;

        ff_channel_layouts_unref(&cfg_in[i]->channel_layouts);
        ret = ff_channel_layouts_ref(in_ch_layout, &cfg_in[i]->channel_layouts);
        if (ret < 0)
            return ret;
    }

    // Copy the activated out_link format to each out_link
    for (i = 0; i < ctx->nb_outputs; i++) {
        ret = ff_add_format(&out_format, priv->out_fmts->format);
        if (ret < 0)
            return ret;

        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(out_format, &cfg_out[i]->formats);
        if (ret < 0)
            return ret;

        ret = ff_add_format(&out_sample_rate, priv->out_fmts->sample_rate);
        if (ret < 0)
            return ret;

        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(out_sample_rate, &cfg_out[i]->samplerates);
        if (ret < 0)
            return ret;

        ret = ff_add_channel_layout(&out_ch_layout, &priv->out_fmts->ch_layout);
        if (ret < 0)
            return ret;

        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(out_ch_layout, &cfg_out[i]->channel_layouts);
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
        FilterLinkInternal *li;
        for (i = 0; i < ctx->nb_inputs; i++) {
            li = ff_link_internal(ctx->inputs[i]);
            av_log(ctx, AV_LOG_INFO, "%s: input[%d]->frame_blocked_in=%d.\n",
               ctx->name, i, li->frame_blocked_in);
        }
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