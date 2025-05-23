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
 * audio alsa src
 */

#include <libavutil/eval.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include "libavutil/time.h"
#include "libavutil/mem.h"

#include <poll.h>

#include "alsa.h"
#include "avfilter.h"
#include "aresample.h"
#include "avfilter_internal.h"
#include "buffersink.h"
#include "buffersrc.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"
#include "volume.h"

#define ROUTE_OFF 0
#define ROUTE_ON 1

typedef struct AlsasrcPriv {
    const AVClass *class;

    AlsaHandle priv;
    char *devname;

    int format;
    uint32_t sample_rate;
    AVChannelLayout ch_layout;

    int periods;
    int period_time;
    int period_size;
    int frame_size;

    char *map_str;
    int *map;
    int nb_outputs;

    int poll_available;

    int64_t timestamp;
    AResampleContext resample;

    VolumeContext vol_ctx;
    int *af_map;
    char *af_map_str;
    char *filter_desc;
    int af_informat;
    int af_outformat;
    int af_insample_rate;
    int af_outsample_rate;
    AVChannelLayout af_inch_layout;
    AVChannelLayout af_outch_layout;
    AVFilterGraph *agraph;
    AVFilterContext *src_filter;
    AVFilterContext *sink_filter;
} AlsasrcPriv;


static int alsasrc_subgraph_dump(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    char* dump;

    if (!priv->agraph)
        return AVERROR(EINVAL);

    dump = avfilter_graph_dump(priv->agraph, NULL);
    if (dump != NULL) {
        av_log(ctx, AV_LOG_INFO, "Filtergraph dump:\n%s\n", dump);
        av_free(dump);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Unable to dump filtergraph\n");
        return AVERROR(ENOMEM);
    }

    return 0;
}

static inline int alsasrc_subgraph_avaliable(AVFilterContext *ctx, int pad)
{
    AlsasrcPriv *priv = ctx->priv;
    return (priv->filter_desc && priv->af_map && priv->af_map[pad]);
}

/**
 * the format for the subgraph command is:
 * agrs = "filtername:sub_cmd:sub_args"
 */
static int alsasrc_subgraph_process_command(AVFilterContext *ctx, const char *args,
                                            char *res, int res_len, int flags)
{
    AlsasrcPriv *priv = ctx->priv;
    char *p, *target, *sub_cmd, *sub_args;
    char *saveptr = NULL;
    int ret = AVERROR(EINVAL);
    int i;

    if(!priv->agraph) {
        av_log(ctx, AV_LOG_WARNING, "subgraph filter not initialized.\n");
        return 0;
    }

    if (!args)
        return AVERROR(EINVAL);

    p = av_strdup(args);
    if (!p)
        return AVERROR(ENOMEM);

    target = strtok_r(p, ":", &saveptr);
    sub_cmd = target ? strtok_r(NULL, ":", &saveptr) : NULL;
    sub_args = sub_cmd ? strtok_r(NULL, ":", &saveptr) : NULL;

    if (!target || !sub_cmd || !sub_args) {
        av_log(ctx, AV_LOG_ERROR, "Invalid format for subgraph command: %s\n", args);
        ret = AVERROR(EINVAL);
        goto end;
    }

    av_log(ctx, AV_LOG_INFO, "subgraph cmd: %s %s %s.\n", target, sub_cmd, sub_args);

    for (i = 0; i < priv->agraph->nb_filters; i++) {
        AVFilterContext *filter = priv->agraph->filters[i];
        if ((filter->name && !strcmp(target, filter->name)) || !strcmp(target, filter->filter->name)) {
            ret = avfilter_process_command(filter, sub_cmd, sub_args, res, res_len, flags);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error executing filter(%s) command %s %s, ret %d\n",
                       target, sub_cmd, args, ret);
            }
            goto end;
        }
    }

    av_log(ctx, AV_LOG_ERROR, "Filter %s not found\n", target);
    ret = AVERROR(ENOENT);

end:
    av_freep(&p);
    return ret;
}

static int alsasrc_subgraph_process(AVFilterContext *ctx, AVFrame *frame)
{
    AlsasrcPriv *priv = ctx->priv;
    int ret;

    if ((ret = av_buffersrc_add_frame_flags(priv->src_filter, frame,
                                            AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Error submitting audio to buffersrc: %s\n",
               av_err2str(ret));
        return ret;
    }

    av_frame_unref(frame);

    if ((ret = av_buffersink_get_frame(priv->sink_filter, frame)) < 0) {
        if (ret != AVERROR(EAGAIN))
            av_log(ctx, AV_LOG_ERROR, "Error while getting the audio from buffersink: %s\n",
                   av_err2str(ret));
        av_free(frame);
        return ret;
    }

    return 0;
}
static int alsasrc_subgraph_config(AVFilterGraph *graph, const char *filter_desc,
                                   AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    AVFilterInOut *outputs = NULL, *inputs = NULL;
    int nb_filters = graph->nb_filters;
    int ret, i;
    if (filter_desc) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;
        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;
        if ((ret = avfilter_graph_parse_ptr(graph, filter_desc, &inputs, &outputs, NULL)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while parsing filtergraph, ret %d\n", ret);
            goto fail;
        }
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while linking filtergraph, ret %d\n", ret);
            goto fail;
        }
    }

    ret = avfilter_graph_config(graph, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while configuring filtergraph, ret %d\n", ret);
        goto fail;
    }

fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}
static int alsasrc_subgraph_init(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AVFilterContext *alg_filter = NULL;
    AVFilterContext *asrc = NULL;
    AVFilterContext *asink = NULL;
    char channel_layout_str[16];
    AVFilterLink *link;
    char args[64];
    int ret;
    int i;

    if (!(priv->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);

    // create source filter
    if ((ret = av_channel_layout_describe(&priv->af_inch_layout, channel_layout_str,
                                          sizeof(channel_layout_str))) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to describe input channel layout.\n");
        goto end;
    }

    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             priv->af_insample_rate, av_get_sample_fmt_name(priv->af_informat),
             channel_layout_str);

    ret = avfilter_graph_create_filter(&asrc, avfilter_get_by_name("abuffer"),
                                       "abuffer", args, NULL, priv->agraph);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Cannot create audio source filter ret %d.\n", ret);
        goto end;
    }

    // create sink filter
    memset(args, 0, sizeof(args));
    memset(channel_layout_str, 0, sizeof(channel_layout_str));
    if ((ret = av_channel_layout_describe(&priv->af_inch_layout, channel_layout_str,
                                          sizeof(channel_layout_str))) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to describe output channel layout.\n");
        goto end;
    }

    snprintf(args, sizeof(args),
             "sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             priv->af_outsample_rate, av_get_sample_fmt_name(priv->af_outformat),
             channel_layout_str);

    ret = avfilter_graph_create_filter(&asink, avfilter_get_by_name("abuffersink"),
                                       "abuffersink", NULL, NULL, priv->agraph);
    if (ret < 0){
        av_log(ctx, AV_LOG_ERROR, "Cannot create audio sink filter ret %d.\n", ret);
        goto end;
    }

    ret = alsasrc_subgraph_config(priv->agraph, priv->filter_desc, asrc, asink);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Cannot create filter graph ret %d.\n", ret);
        goto end;
    }

    priv->src_filter = asrc;
    priv->sink_filter = asink;
    return 0;
end:
    if (priv->agraph)
        avfilter_graph_free(&priv->agraph);
    return ret;
}

static void alsasrc_subgraph_uninit(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    if (priv->agraph)
        avfilter_graph_free(&priv->agraph);
}

static inline void alsasrc_force_request(AVFilterContext *ctx)
{
    AlsasrcPriv *s = ctx->priv;
    FilterLinkInternal *li;
    int i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (s->map && s->map[i] == ROUTE_OFF)
            continue;

        li = ff_link_internal(ctx->outputs[i]);
        li->frame_wanted_out = 1;
        ff_filter_set_ready(ctx, 300);
    }
}

static int alsasrc_get_device_support_format(AVFilterContext *ctx, const char *devname,
                                             const char *key, int value, int *out_value)
{
    AVOptionRanges* ranges = NULL;
    AVOptionRange* range = NULL;
    int ret, range_idx;

    ret = alsa_query_caps(&ranges, devname, key, false);
    if (ret > 0) {
        for (range_idx = 0; range_idx < ranges->nb_ranges; range_idx++) {
            range = ranges->range[range_idx];
            if (value >= range->value_min && value <= range->value_max)
                break;
        }

        *out_value = range_idx == ranges->nb_ranges ? ranges->range[0]->value_min : value;
        av_opt_freep_ranges(&ranges);
    } else {
        av_log(ctx, AV_LOG_ERROR, "Unsupported query %s: %d\n", key, value);
        return AVERROR(EINVAL);
    }

    return 0;
}

static int alsasrc_config_formats(AVFilterLink *link, int pad)
{
    AVFilterContext *ctx = link->src;
    AlsasrcPriv *priv = ctx->priv;
    AVChannelLayout dst_chan_layout;
    int dst_sample_rate;
    int dst_format;
    int ret;

    /* if subgragh is enable, use the subgraph input format to negotiate with alsasrc dev,
       or use out link format to negotiate with alsasrc dev */
    if (alsasrc_subgraph_avaliable(ctx, pad)) {
        dst_sample_rate = priv->af_insample_rate;
        dst_chan_layout = priv->af_inch_layout;
        dst_format = priv->af_informat;

        // if output format is not configured, use link format
        if (priv->af_outformat == -1)
            priv->af_outformat = link->format;

        if (priv->af_outsample_rate == -1)
            priv->af_outsample_rate = link->sample_rate;

        if (priv->af_outch_layout.nb_channels == 0)
            av_channel_layout_copy(&priv->af_outch_layout, &link->ch_layout);
    } else {
        dst_sample_rate = link->sample_rate;
        dst_chan_layout = link->ch_layout;
        dst_format = link->format;
    }

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_fmts",
                                            dst_format, &priv->format);
    if (ret < 0)
        return ret;

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_rates",
                                            dst_sample_rate, &priv->sample_rate);
    if (ret < 0)
        return ret;

    ret = alsasrc_get_device_support_format(ctx, priv->devname, "channels",
                                            dst_chan_layout.nb_channels, &priv->ch_layout.nb_channels);
    if (ret < 0)
        return ret;

    if (priv->ch_layout.nb_channels == dst_chan_layout.nb_channels)
        av_channel_layout_copy(&priv->ch_layout, &dst_chan_layout);
    else
        av_channel_layout_default(&priv->ch_layout, priv->ch_layout.nb_channels);

    if (alsasrc_subgraph_avaliable(ctx, pad)) {
        priv->af_insample_rate = priv->sample_rate;
        priv->af_informat = priv->format;
        av_channel_layout_copy(&priv->af_inch_layout, &priv->ch_layout);
    }

    return 0;
}

static void alsasrc_close(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;

    alsa_close(&priv->priv);

    volume_uninit(&priv->vol_ctx);
    alsasrc_subgraph_uninit(ctx);
}

static int alsasrc_init_dict(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
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

    if (priv->map_str) {
        ret = avfilter_parse_mapping(priv->map_str, &priv->map, priv->nb_outputs);
        if (ret < 0)
            return ret;
    }

    if (priv->af_map_str) {
        ret = avfilter_parse_mapping(priv->af_map_str, &priv->af_map, priv->nb_outputs);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "parse af_map_str failed ret %d.\n", ret);
            return ret;
        }
    }

    ff_resample_init(&priv->resample);

    return 0;
}

static void alsasrc_uninit(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    int i;

    ff_resample_uninit(&priv->resample);

    av_freep(&priv->map);
}

static int alsasrc_query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in,
                                 AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    int ret, i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
        ff_formats_unref(&cfg_in[i]->formats);
        ret = ff_formats_ref(formats, &cfg_in[i]->formats);
        if (ret < 0)
            goto out;

        formats = ff_all_samplerates();
        ff_formats_unref(&cfg_in[i]->samplerates);
        ret = ff_formats_ref(formats, &cfg_in[i]->samplerates);
        if (ret < 0)
            goto out;

        layouts = ff_all_channel_counts();
        ff_channel_layouts_unref(&cfg_in[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_in[i]->channel_layouts);
        if (ret < 0)
            goto out;
    }

out:
    return ret;
}

static void alsasrc_set_eof(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    int i;

    for (i = 0; i < ctx->nb_outputs; i++)
        ff_outlink_set_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
}

static int alsasrc_check_outlink_status(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int need_close = 1;
    int i, ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ff_outlink_get_status(ctx->outputs[i]) != AVERROR_EOF) {
            need_close = 0;
            break;
        }
    }

    if (need_close && handle->h) {
        alsasrc_close(ctx);
        return 0;
    }

    return 1;
}

static int alsasrc_get_parameter(AVFilterContext *ctx, const char *key, char *value, int len)
{
    AlsasrcPriv *priv = ctx->priv;
    int ret;

    if (!strcmp(key, "format")) {
        // return the default format of the alsasrc dev
        ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_fmts",
                                                -1, &priv->format);
        if (ret < 0)
            return ret;

        ret = alsasrc_get_device_support_format(ctx, priv->devname, "sample_rates",
                                                priv->sample_rate, &priv->sample_rate);
        if (ret < 0)
            return ret;

        ret = alsasrc_get_device_support_format(ctx, priv->devname, "channels",
                                                0, &priv->ch_layout.nb_channels);
        if (ret < 0)
            return ret;

        snprintf(value, len, "fmt=%d:rate=%d:ch=%d",
                 priv->format, priv->sample_rate, priv->ch_layout.nb_channels);

        return 0;
    } else if (!strcmp(key, "volume")) {
        snprintf(value, len, "vol:%f", priv->vol_ctx.volume);

        av_log(priv, AV_LOG_INFO, "get_parameter: %s = %.2f\n", key, priv->vol_ctx.volume);
        return 0;
    }

    av_log(ctx, AV_LOG_ERROR, "get_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

static int alsasrc_set_parameter(AVFilterContext *ctx, const char *args)
{
    AlsasrcPriv *priv = ctx->priv;
    char *key = NULL, *value = NULL;
    const char *p = args;
    int ret = 0;

    av_log(ctx, AV_LOG_INFO, "Parsing args: %s\n", args);

    while (*p) {
        ret = av_opt_get_key_value(&p, "=", ":", 0, &key, &value);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "No more key-value pairs to parse.\n");
            break;
        }

        if (*p)
            p++;

        av_log(ctx, AV_LOG_INFO, "Parsed Key: %s, Value: %s\n", key, value);

        if (!strcmp(key, "sample_rate")) {
            priv->sample_rate = atoi(value);
            av_log(ctx, AV_LOG_INFO, "Set sample_rate to %d\n", priv->sample_rate);
        } else if (!strcmp(key, "volume")) {
            double volume;

            ret = av_expr_parse_and_eval(&volume, value, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "Error when parsing %s volume expression '%s'\n", ctx->name, value);
                goto end;
            }

            volume_set(&priv->vol_ctx, volume);

            av_log(priv, AV_LOG_INFO, "set_parameter: %s = %.2f\n", key, priv->vol_ctx.volume);
        } else
            av_log(ctx, AV_LOG_ERROR, "Unknown parameter: %s\n", key);

end:
        av_freep(&key);
        av_freep(&value);
    }

    return ret;
}

static int alsasrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    int ret;

    if (!strcmp(cmd, "link")) {
        alsasrc_force_request(ctx);
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        if (handle->h) {
            alsasrc_close(ctx);
            alsasrc_set_eof(ctx);
        }

        return 0;
    } else if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll = (struct pollfd *)res;
        int ret;

        if (!res || res_len < sizeof(struct pollfd))
            return AVERROR(EINVAL);

        if (!handle->h || priv->poll_available >= handle->periods)
            return 0;

        ret = snd_pcm_poll_descriptors(handle->h, poll, 1);

        if (ret < 0)
            return 0;

        return 1;
    } else if (!strcmp(cmd, "poll_available")) {
        ff_filter_set_ready(ctx, 100);
        priv->poll_available++;
        return 0;
    } else if (!strcmp(cmd, "map")) {
        ret = avfilter_parse_mapping(args, &priv->map, priv->nb_outputs);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to parse mapping: %s ret:%d\n", args, ret);
            return ret;
        }

        for (int i = 0; i < ctx->nb_outputs; i++) {
            AVFilterLink *link = ctx->outputs[i];
            if (priv->map && priv->map[i] == ROUTE_OFF)
            {
                av_log(ctx, AV_LOG_INFO, "disable output%d\n", i);
                ff_inlink_set_status(link, AVERROR_EOF);
            }
        }

        ff_filter_set_ready(ctx, 100);
        return ret;
    } else if (!strcmp(cmd, "get_parameter")) {
        if (!args || res_len <= 0)
            return AVERROR(EINVAL);

        return alsasrc_get_parameter(ctx, args, res, res_len);
    } else if (!strcmp(cmd, "set_parameter")) {
        if (!args)
            return AVERROR(EINVAL);

        return alsasrc_set_parameter(ctx, args);
    } else if (!strcmp(cmd, "dump")) {
        alsasrc_subgraph_dump(ctx);
        return 0;
    }  else if (!strcmp(cmd, "af_cmd")) {
        return alsasrc_subgraph_process_command(ctx, args, res, res_len, flags);
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int alsasrc_read_frame(AlsasrcPriv *priv, AVFrame **frame)
{
    AlsaHandle *handle = &priv->priv;
    AVFrame *src;
    int ret;

    src = av_frame_alloc();
    if (!src) {
        av_log(priv, AV_LOG_ERROR, "Failed to allocate frame.\n");
        return AVERROR(ENOMEM);
    }

    src->format = priv->format;
    src->sample_rate = priv->sample_rate;
    av_channel_layout_copy(&src->ch_layout, &priv->ch_layout);
    src->nb_samples = priv->period_size / priv->ch_layout.nb_channels;

    ret = av_frame_get_buffer(src, 0);
    if (ret < 0) {
        av_log(priv, AV_LOG_ERROR, "Failed to allocate frame buffer, ret %d.\n", ret);
        goto fail;
    }

    ret = alsa_read(handle, src->data[0], priv->period_size);
    if (ret < 0)
        goto fail;

    src->pkt_size = ret * handle->frame_size;
    src->nb_samples = ret / priv->ch_layout.nb_channels;
    src->linesize[0] = src->pkt_size;
    src->pts = av_rescale_q(priv->timestamp, (AVRational){1, priv->sample_rate},
                            (AVRational){1, 1000000});

    priv->timestamp += ret;
    priv->poll_available = 0;

    *frame = src;
    return ret;

fail:
    av_frame_free(&src);
    return ret;
}

static int alsasrc_open(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    AVFilterLink *outlink = NULL;
    int ret;
    int pad;
    int i;

    if (handle->h)
        return 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        pad = i;
        outlink = ctx->outputs[i];
        if (priv->af_map && priv->af_map[i])
            break;
    }

    if (!outlink)
        return AVERROR(EINVAL);

    ret = alsasrc_config_formats(outlink, pad);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "config formats failed %d.\n", ret);
        goto error;
    }

    if (alsasrc_subgraph_avaliable(ctx, pad)) {
       ret = alsasrc_subgraph_init(ctx);
       if (ret < 0) {
           av_log(ctx, AV_LOG_ERROR, "subgraph(%s) init failed %d.\n", priv->filter_desc, ret);
           goto error;
       }
    }

    priv->period_size = priv->period_time * priv->sample_rate / 1000;
    handle->periods = priv->periods;
    handle->period_time = priv->period_time;

    ret = alsa_open(handle, priv->devname, SND_PCM_STREAM_CAPTURE, priv->sample_rate,
                    priv->ch_layout.nb_channels, priv->format);
    if (ret < 0)
        return ret;

    priv->timestamp = 0;

    ret = volume_init(&priv->vol_ctx, priv->format);
    if (ret < 0)
        goto error;

    return 0;

error:
    alsasrc_close(ctx);
    return ret;
}

static int alsasrc_activate(AVFilterContext *ctx)
{
    AlsasrcPriv *priv = ctx->priv;
    AlsaHandle *handle = &priv->priv;
    AVFrame *frame = NULL;
    int i, ret;

    ret = alsasrc_check_outlink_status(ctx);
    if (!ret)
        return ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ff_outlink_frame_wanted(ctx->outputs[i])) {
            ret = alsasrc_open(ctx);
            if (ret < 0)
                goto out;

            break;
        }
    }

    if (i == ctx->nb_outputs)
       return FFERROR_NOT_READY;

    ret = alsasrc_read_frame(priv, &frame);
    if (ret < 0)
        goto out;

    for (i = 0; i < ctx->nb_outputs; i++) {
        AVFrame *oframe = NULL;
        AVFrame *iframe = NULL;
        AVFilterLink *link;

        if (priv->map && priv->map[i] == ROUTE_OFF)
            continue;

        iframe = av_frame_clone(frame);
        if (!iframe) {
            av_frame_free(&frame);
            ret = AVERROR(ENOMEM);
            goto out;
        }

        if (priv->agraph && priv->af_map && priv->af_map[i]) {
            ret = alsasrc_subgraph_process(ctx, iframe);
            if (ret < 0)
                continue;
        }

        volume_scale(&priv->vol_ctx, iframe);

        link = ctx->outputs[i];
        ret = ff_resample_frame(&priv->resample, link, iframe, &oframe);
        if (ret == 0)
            oframe = iframe;
        else
            av_frame_free(&iframe);

        ret = ff_filter_frame(link, oframe);
        if (ret < 0)
            goto out;
    }

    av_frame_free(&frame);
out:

    return ret;
}

#define OFFSET(x) offsetof(AlsasrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption alsasrc_options[] = {
    { "devname",           "", OFFSET(devname),           AV_OPT_TYPE_STRING,     .flags = A },
    { "sample_rate",       "", OFFSET(sample_rate),       AV_OPT_TYPE_INT,        {.i64 = 0},                  0, INT_MAX, R },
    { "ch_layout",         "", OFFSET(ch_layout),         AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},               0, 0,       R },
    { "periods",           "", OFFSET(periods),           AV_OPT_TYPE_INT,        {.i64 = 4},                  0, INT_MAX, R },
    { "period_time",       "", OFFSET(period_time),       AV_OPT_TYPE_INT,        {.i64 = 20},                 0, INT_MAX, R },
    { "outputs",           "", OFFSET(nb_outputs),        AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "map",               "", OFFSET(map_str),           AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "map_array",         "", OFFSET(map),               AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { "af",                "", OFFSET(filter_desc),       AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "af_map",            "", OFFSET(af_map_str),        AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "af_informat",       "", OFFSET(af_informat),       AV_OPT_TYPE_INT,        {.i64 = -1},                  -1, INT_MAX, R },
    { "af_insample_rate",  "", OFFSET(af_insample_rate),  AV_OPT_TYPE_INT,        {.i64 = -1},                  -1, INT_MAX, R },
    { "af_inch_layout",    "", OFFSET(af_inch_layout),    AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},               0, 0,       R },
    { "af_outformat",      "", OFFSET(af_outformat),      AV_OPT_TYPE_INT,        {.i64 = -1},                  -1, INT_MAX, R },
    { "af_outsample_rate", "", OFFSET(af_outsample_rate), AV_OPT_TYPE_INT,        {.i64 = -1},                  -1, INT_MAX, R },
    { "af_outch_layout",   "", OFFSET(af_outch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},               0, 0,       R },
    { NULL },
};

static const AVClass alsasrc_class = {
    .class_name          = "alsasrc_class",
    .item_name           = av_default_item_name,
    .option              = alsasrc_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
};

const AVFilter ff_asrc_alsasrc = {
    .name            = "alsasrc",
    .description     = NULL_IF_CONFIG_SMALL("Audio alsa src"),
    .priv_class      = &alsasrc_class,
    .priv_size       = sizeof(AlsasrcPriv),
    .init            = alsasrc_init_dict,
    .uninit          = alsasrc_uninit,
    FILTER_QUERY_FUNC2(alsasrc_query_formats),
    .activate        = alsasrc_activate,
    .process_command = alsasrc_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
