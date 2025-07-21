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
 * audio device source
 */

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat_internal.h>
#include <libavformat/demux.h>
#include <libavutil/avstring.h>
#include <libavformat/internal.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include "libavcodec/bytestream.h"

#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"
#include "internal.h"
#include "libavutil/mem.h"
#include "mapping.h"

#define AVFMT_FLAG_CODEC_READY    0x20000

typedef int (*fmt_control_msg)(struct AVFormatContext *s1, int type,
                               void *data, size_t data_size);

typedef struct ADevSrcPriv {
    const AVClass   *class;

    AVFormatContext *fmt_ctx;
    AVCodecContext  *dec_ctx;

    char            *format;
    char            *devname;

    int             sample_fmt;
    uint32_t        sample_rate;
    AVChannelLayout ch_layout;

    char            *map_str;
    int             *map;
    int             nb_outputs;
} ADevSrcPriv;

static inline void adevsrc_force_request(AVFilterContext *ctx)
{
    FilterLinkInternal *li = ff_link_internal(ctx->outputs[0]);
    li->frame_wanted_out = 1;
    ff_filter_set_ready(ctx, 300);
}

static int adevsrc_control_message(struct AVFormatContext *s, int type,
                                    void *data, size_t data_size)
{
    AVFilterContext *ctx = s->opaque;

    if (type == AV_DEV_TO_APP_STATE_CHANGED ||
        type == AV_DEV_TO_APP_BUFFER_READABLE) {
            adevsrc_force_request(ctx);
    }

    return 0;
}

int adevsrc_read_close(AVFormatContext *s)
{
    const FFInputFormat *iformat;

    if (!s || !s->iformat)
        return AVERROR(EINVAL);

    ff_flush_packet_queue(s);

    iformat = ffifmt(s->iformat);
    if (iformat->read_close) {
        iformat->read_close(s);
        s->flags &= (~AVFMT_FLAG_CODEC_READY);
    }

    return 0;
}

static void adevsrc_close(AVFilterContext *ctx)
{
    ADevSrcPriv *priv = ctx->priv;
    const FFInputFormat *iformat;

    if (!priv->dec_ctx)
        return;

    ff_flush_packet_queue(priv->fmt_ctx);

    iformat = ffifmt(priv->fmt_ctx->iformat);
    if (iformat->read_close) {
        iformat->read_close(priv->fmt_ctx);
        priv->fmt_ctx->flags &= (~AVFMT_FLAG_CODEC_READY);
    }

    avcodec_free_context(&priv->dec_ctx);
    priv->dec_ctx = NULL;
}

static int adevsrc_config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    ADevSrcPriv *priv = ctx->priv;
    AVDictionary *fmt_opt = NULL;
    char tmp[64];
    int ret;

    av_dict_set_int(&fmt_opt, "sample_rate", link->sample_rate, 0);
    av_channel_layout_describe(&link->ch_layout, tmp, sizeof(tmp));
    av_dict_set(&fmt_opt, "ch_layout", tmp, 0);

    ret = av_opt_set_dict(priv->fmt_ctx->priv_data, &fmt_opt);

    av_dict_free(&fmt_opt);

    return ret;
}

static int adevsrc_open(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->outputs[0];
    ADevSrcPriv *priv  = ctx->priv;
    const FFInputFormat *iformat;
    const AVCodec *dec;
    AVStream *st;
    int ret;

    if (priv->dec_ctx)
        return 0;

    adevsrc_config_props(link);

    priv->fmt_ctx->flags |= AVFMT_FLAG_CODEC_READY;
    iformat = ffifmt(priv->fmt_ctx->iformat);
    ret = iformat->read_header(priv->fmt_ctx);
    if (ret < 0)
        return ret;

    st = priv->fmt_ctx->streams[0];
    if (!st)
        goto out;

    st->time_base = (AVRational){ 1, st->codecpar->sample_rate };

    /* Find decoder for the stream */
    dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec)
        goto out;

    /* Allocate a codec context for the decoder */
    priv->dec_ctx = avcodec_alloc_context3(dec);
    if (!priv->dec_ctx) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    /* Copy codec parameters from input stream to output codec context */
    ret = avcodec_parameters_to_context(priv->dec_ctx, st->codecpar);
    if (ret < 0)
        goto out;

    /* Init the decoders */
    ret = avcodec_open2(priv->dec_ctx, dec, NULL);
    if (ret < 0)
        goto out;

    return 0;

out:
    adevsrc_close(ctx);
    return ret;
}

static int adevsrc_init_dict(AVFilterContext *ctx)
{
    const AVInputFormat *fmt = NULL;
    ADevSrcPriv *priv = ctx->priv;
    int ret;
    int i;

    fmt = av_find_input_format(priv->format);
    if (!fmt)
        return AVERROR(EINVAL);

    priv->fmt_ctx = avformat_alloc_context();
    if (!priv->fmt_ctx)
        return AVERROR(ENOMEM);

    priv->fmt_ctx->opaque             = ctx;
    priv->fmt_ctx->control_message_cb = adevsrc_control_message;
    priv->fmt_ctx->flags             |= AVFMT_FLAG_NONBLOCK;

    ret = avformat_open_input(&priv->fmt_ctx, priv->devname, fmt, NULL);
    if (ret < 0) {
        avformat_free_context(priv->fmt_ctx);
        priv->fmt_ctx = NULL;
        return ret;
    }

    for (i = 0; i < priv->nb_outputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        pad.config_props = adevsrc_config_props;
        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    if (priv->map_str) {
        ret = avfilter_parse_mapping(priv->map_str, &priv->map, priv->nb_outputs);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static void adevsrc_uninit(AVFilterContext *ctx)
{
    ADevSrcPriv *priv = ctx->priv;

    av_freep(&priv->map);
    adevsrc_close(ctx);
    avformat_close_input(&priv->fmt_ctx);
}

static int adevsrc_receive_frame(AVFilterContext *ctx, AVFrame **frame)
{
    AVFilterLink *link = ctx->outputs[0];
    ADevSrcPriv *priv = ctx->priv;
    AVFrame *out;
    int ret;

    out = av_frame_alloc();
    if (!out)
        return AVERROR(ENOMEM);

    while (1) {
        AVPacket pkt1, *pkt = &pkt1;

        ret = avcodec_receive_frame(priv->dec_ctx, out);
        if (ret >= 0)
            break;
        else if (ret != AVERROR(EAGAIN))
            goto error;

        ret = ff_read_packet(priv->fmt_ctx, pkt);
        if (ret < 0)
            goto error;

        ret = avcodec_send_packet(priv->dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            goto error;
    }

    av_channel_layout_copy(&out->ch_layout, &link->ch_layout);
    *frame = out;
    return 0;

error:
    av_frame_free(&out);
    return ret;
}

static int adevsrc_send_empty_frame(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->outputs[0];
    AVFrame *frame;
    int ret;

    frame = av_frame_alloc();
    if (!frame)
      return AVERROR(ENOMEM);

    frame->nb_samples = 0;
    frame->format = link->format;
    frame->sample_rate = link->sample_rate;
    av_channel_layout_copy(&frame->ch_layout, &link->ch_layout);

    if ((ret = ff_filter_frame(link, frame)) < 0)
      av_log(ctx, AV_LOG_ERROR, "send empty frame failed:%d\n", ret);

    return ret;
}

static int adevsrc_activate(AVFilterContext *ctx)
{
    AVFilterLink *link = ctx->outputs[0];
    ADevSrcPriv *priv = ctx->priv;
    AVFrame *frame;
    int ret;

    if (!ff_outlink_frame_wanted(link))
        return FFERROR_NOT_READY;

    ret = adevsrc_open(ctx);
    if (ret < 0)
        goto out;

    ret = adevsrc_receive_frame(ctx, &frame);
    if (ret < 0)
        goto out;

    return ff_filter_frame(link, frame);

out:
    if (ret == AVERROR_EOF) {
        adevsrc_send_empty_frame(ctx);
        adevsrc_close(ctx);
        ret = 0;
    } else if (ret < 0 && ret != AVERROR(EAGAIN))
        ff_filter_set_ready(ctx, 300);

    return ret;
}

static int adevsrc_get_control_message(AVFilterContext *ctx, fmt_control_msg *cb)
{
    ADevSrcPriv *priv = ctx->priv;
    AVOptionRanges *ranges = NULL;
    int ret;

    ret = av_opt_query_ranges2(&ranges, priv->fmt_ctx->priv_data, priv->fmt_ctx,
                               "control_message", AV_OPT_MULTI_COMPONENT_RANGE);
    if (ret == 0 && ranges->nb_ranges == 0) {
        *cb = (fmt_control_msg)ranges->range;
        ranges->range = NULL;
        av_opt_freep_ranges(&ranges);
        return 0;
    }

    av_opt_freep_ranges(&ranges);
    return AVERROR(EPERM);
}

static int adevsrc_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                   char *res, int res_len, int flags)
{
    fmt_control_msg avdevice_app_to_dev_control_message;
    ADevSrcPriv *priv = ctx->priv;
    int ret;

    ret = adevsrc_get_control_message(ctx, &avdevice_app_to_dev_control_message);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "get control_message failed\n");
        return ret;
    }

    if (!strcmp(cmd, "start") || !strcmp(cmd, "link")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_START,
                res, res_len);
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "unlink")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_STOP,
                res, res_len);
    } else if (!strcmp(cmd, "get_pollfd")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_GET_POLLFD,
                res, res_len);
    } else if (!strcmp(cmd, "poll_available")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_POLL_AVAILABLE,
                res, res_len);
    } else if (!strcmp(cmd, "set_parameter")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_SET_PARAMETER,
                (char *)args, 0);
    } else if (!strcmp(cmd, "mute")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_MUTE,
                NULL, 0);
    } else if (!strcmp(cmd, "unmute")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_UNMUTE,
                NULL, 0);
    } else if (!strcmp(cmd, "dump")) {
        return avdevice_app_to_dev_control_message(
                priv->fmt_ctx,
                AV_APP_TO_DEV_DUMP,
                res, res_len);
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int adevsrc_query_formats(const AVFilterContext *ctx,
                                 AVFilterFormatsConfig **cfg_in,
                                 AVFilterFormatsConfig **cfg_out)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats = NULL;
    ADevSrcPriv *priv = ctx->priv;
    AVOptionRanges *ranges = NULL;
    AVChannelLayout layout;
    int ret, i, j;

    for (i = 0; i < ctx->nb_outputs; i++) {
        int list[] = { 0, -1 };

        if (priv->sample_fmt != AV_SAMPLE_FMT_NONE) {
            list[0] = priv->sample_fmt;
            formats = ff_make_format_list(list);
            if (!formats)
                goto out;
        } else {
            ret = av_opt_query_ranges2(&ranges, priv->fmt_ctx->priv_data,
                                       priv->fmt_ctx, "sample_fmts",
                                       AV_OPT_MULTI_COMPONENT_RANGE);
            if (ret >= 0) {
                for (j = 0; j < ranges->nb_ranges; j++) {
                    ret = ff_add_format(&formats, ranges->range[j]->value_min);
                    if (ret < 0)
                        goto out;
                }
                av_opt_freep_ranges(&ranges);
            } else {
                formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
            }
        }

        ff_formats_unref(&cfg_out[i]->formats);
        ret = ff_formats_ref(formats, &cfg_out[i]->formats);
        if (ret < 0)
            goto out;

        formats = NULL;

        if (priv->sample_rate) {
            list[0] = priv->sample_rate;
            formats = ff_make_format_list(list);
            if (!formats)
                goto out;
        } else {
            ret = av_opt_query_ranges2(&ranges, priv->fmt_ctx->priv_data,
                                       priv->fmt_ctx, "sample_rates",
                                       AV_OPT_MULTI_COMPONENT_RANGE);
            if (ret >= 0) {
                for (j = 0; j < ranges->nb_ranges; j++) {
                    ret = ff_add_format(&formats, ranges->range[j]->value_min);
                    if (ret < 0)
                        goto out;
                }
                av_opt_freep_ranges(&ranges);
            } else {
                formats = ff_all_samplerates();
            }
        }

        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(formats, &cfg_out[i]->samplerates);
        if (ret < 0)
            goto out;

        if (priv->ch_layout.nb_channels) {
            AVChannelLayout list64[] = { { 0 }, { 0 } };

            list64[0] = priv->ch_layout;
            layouts = ff_make_channel_layout_list(list64);
            if (!layouts)
                goto out;
        } else {
            ret = av_opt_query_ranges2(&ranges, priv->fmt_ctx->priv_data,
                                       priv->fmt_ctx, "channels",
                                       AV_OPT_MULTI_COMPONENT_RANGE);
            if (ret >= 0) {
                int ch;

                for (j = 0; j < ranges->nb_ranges; j++) {
                    if (ranges->range[j]->is_range) {
                        for (ch = ranges->range[0]->value_min; ch <= ranges->range[0]->value_max; ch++) {
                            av_channel_layout_default(&layout, ch);
                            ret = ff_add_channel_layout(&layouts, &layout);
                            if (ret < 0)
                                goto out;
                        }
                    } else {
                        ch = ranges->range[j]->value_min;
                        av_channel_layout_default(&layout, ch);
                        ret = ff_add_channel_layout(&layouts, &layout);
                        if (ret < 0)
                            goto out;
                    }
                }

                av_opt_freep_ranges(&ranges);
            } else {
                layouts = ff_all_channel_counts();
            }
        }

        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(layouts, &cfg_out[i]->channel_layouts);
    }

out:
    av_opt_freep_ranges(&ranges);
    return ret;
}

static void *adevsrc_child_next(void *obj, void *prev)
{
    ADevSrcPriv *priv = obj;

    if (!prev)
        return priv->fmt_ctx;
    else if (prev == priv->fmt_ctx)
        return priv->dec_ctx;
    else
        return NULL;
}

static const struct AVClass *adevsrc_child_class_iterate(void **iter)
{
    const AVClass *c = *iter;

    if (!c)
        c = avformat_get_class();
    else if (c == avformat_get_class())
        c = avcodec_get_class();
    else
        c = NULL;

    *iter = (void*)(uintptr_t)c;
    return *iter;
}

#define OFFSET(x) offsetof(ADevSrcPriv, x)
#define A AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption adevsrc_options[] = {
    { "format",      "", OFFSET(format),      AV_OPT_TYPE_STRING,     .flags = A },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = A },
    { "sample_fmt",  "", OFFSET(sample_fmt),  AV_OPT_TYPE_SAMPLE_FMT, {.i64=AV_SAMPLE_FMT_NONE}, -1, INT_MAX, R },
    { "sample_rate", "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                 0, INT_MAX, R },
    { "ch_layout",   "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},              0, 0,       R },
    { "outputs",     "", OFFSET(nb_outputs),  AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "map",         "", OFFSET(map_str),     AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "map_array",   "", OFFSET(map),         AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { NULL },
};

static const AVClass adevsrc_class = {
    .class_name          = "adevsrc_class",
    .item_name           = av_default_item_name,
    .option              = adevsrc_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = adevsrc_child_next,
    .child_class_iterate = adevsrc_child_class_iterate,
};

const AVFilter ff_asrc_adevsrc = {
    .name            = "adevsrc",
    .description     = NULL_IF_CONFIG_SMALL("audio device source"),
    .priv_size       = sizeof(ADevSrcPriv),
    .priv_class      = &adevsrc_class,
    .init            = adevsrc_init_dict,
    .uninit          = adevsrc_uninit,
    FILTER_QUERY_FUNC2(adevsrc_query_formats),
    .activate        = adevsrc_activate,
    .process_command = adevsrc_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
