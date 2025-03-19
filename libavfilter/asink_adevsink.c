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
 * audio device sink
 */

#include <libavutil/opt.h>
#include <libavutil/eval.h>
#include <libavutil/samplefmt.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavformat/avformat_internal.h>
#include <libavcodec/avcodec.h>
#include <libavformat/mux.h>

#include "filters.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "formats.h"
#include "internal.h"

typedef struct ADevSinkPriv {
    const AVClass   *class;

    AVFormatContext *fmt_ctx;
    AVCodecContext  *enc_ctx;

    char            *format;
    char            *devname;

    int             sample_fmt;
    uint32_t        sample_rate;
    AVChannelLayout ch_layout;
    bool            started;

    AVPacket        *last_pkt;
} ADevSinkPriv;

typedef struct AVOptionRanges AVOptionRanges;

//libavformat mux.h externs
int write_packets_from_bsfs(AVFormatContext *s, AVStream *st, AVPacket *pkt, int interleaved);
int interleaved_write_packet(AVFormatContext *s, AVPacket *pkt,
                                    int flush, int has_packet);

static int adevsink_control_message(struct AVFormatContext *s, int type,
                                    void *data, size_t data_size)
{
    AVFilterContext *ctx = s->opaque;

    if (type == AV_DEV_TO_APP_BUFFER_WRITABLE)
        ff_filter_set_ready(ctx, 100);
    else if (type == AV_DEV_TO_APP_STATE_CHANGED) {
        ff_filter_set_ready(ctx, 100);
    }

    return 0;
}

static int adevsink_open_encoder(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    ADevSinkPriv *priv = ctx->priv;
    AVStream *st = priv->fmt_ctx->streams[0];
    AVDictionary *fmt_opt = NULL;
    AVDictionary *dict = NULL;
    enum AVCodecID codec_id;
    const AVCodec *enc;
    char *param;
    int ret;

    codec_id = priv->fmt_ctx->oformat->audio_codec != AV_CODEC_ID_NONE ?
               priv->fmt_ctx->oformat->audio_codec : priv->fmt_ctx->audio_codec_id;

    enc = avcodec_find_encoder(codec_id);
    if (!enc)
        return AVERROR(EINVAL);

    priv->enc_ctx = avcodec_alloc_context3(enc);
    if (!priv->enc_ctx)
        return AVERROR(ENOMEM);

    priv->enc_ctx->codec_type  = inlink->type;
    priv->enc_ctx->sample_fmt  = inlink->format;
    priv->enc_ctx->sample_rate = inlink->sample_rate;
    av_channel_layout_copy(&priv->enc_ctx->ch_layout, &inlink->ch_layout);

    av_dict_set_int(&fmt_opt, "ar", inlink->sample_rate, 0);
    av_dict_set_int(&fmt_opt, "ac", inlink->ch_layout.nb_channels, 0);
    /* channel_layout device->avctx->codec */
    avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                        AV_APP_TO_DEV_GET_FORMAT_REQUEST,
                                        &fmt_opt, sizeof(AVDictionary *));

    ret = avcodec_open2(priv->enc_ctx, enc, &fmt_opt);
    av_dict_free(&fmt_opt);
    if (ret < 0) {
        avcodec_free_context(&priv->enc_ctx);
        return ret;
    }

    st->time_base = (AVRational){ 1, inlink->sample_rate };
    avcodec_parameters_from_context(st->codecpar, priv->enc_ctx);

    return 0;
}

static void adevsink_close_encoder(AVFilterContext *ctx)
{
    ADevSinkPriv *priv = ctx->priv;

    if (!priv->enc_ctx)
        return;

    avcodec_free_context(&priv->enc_ctx);
}

static int avformat_write_trailer(AVFormatContext *s)
{
    FFFormatContext *const si = ffformatcontext(s);
    AVPacket *const pkt = si->parse_pkt;
    int ret1, ret = 0;

    for (unsigned i = 0; i < s->nb_streams; i++) {
        AVStream *const st = s->streams[i];
        FFStream *const sti = ffstream(st);
        if (sti->bsfc) {
            ret1 = write_packets_from_bsfs(s, st, pkt, 1 /*interleaved*/);
            if (ret1 < 0)
                av_packet_unref(pkt);
            if (ret >= 0)
                ret = ret1;
        }
    }
    ret1 = interleaved_write_packet(s, pkt, 1, 0);
    if (ret >= 0)
        ret = ret1;

    if (ffofmt(s->oformat)->write_trailer) {
        if (!(s->oformat->flags & AVFMT_NOFILE) && s->pb)
            avio_write_marker(s->pb, AV_NOPTS_VALUE, AVIO_DATA_MARKER_TRAILER);
        ret1 = ffofmt(s->oformat)->write_trailer(s);
        if (ret >= 0)
            ret = ret1;
    }

    return ret;
}

static int adevsink_start(AVFilterContext *ctx)
{
    ADevSinkPriv *priv = ctx->priv;
    int ret;

    if (priv->started)
        return 0;

    priv->last_pkt = av_packet_alloc();
    if (!priv->last_pkt)
        return -ENOMEM;

    ret = adevsink_open_encoder(ctx);
    if (ret < 0) {
        av_packet_free(&priv->last_pkt);
        return ret;
    }

    ret = avformat_write_header(priv->fmt_ctx, NULL);
    if (ret < 0) {
        avcodec_free_context(&priv->enc_ctx);
        av_packet_free(&priv->last_pkt);
        return ret;
    }

    priv->started = true;
    return 0;
}

static void adevsink_stop(AVFilterContext *ctx)
{
    ADevSinkPriv *priv = ctx->priv;

    if (!priv->started)
        return;

    if (priv->last_pkt)
        av_packet_free(&priv->last_pkt);

    avformat_write_trailer(priv->fmt_ctx);
    adevsink_close_encoder(ctx);

    priv->started = false;
}

static int adevsink_init_dict(AVFilterContext *ctx)
{
    ADevSinkPriv *priv = ctx->priv;
    AVDictionary **options = NULL;
    AVStream *st;
    int ret;

    ret = avformat_alloc_output_context2(&priv->fmt_ctx, NULL,
                                         priv->format, priv->devname);
    if (ret < 0)
        return ret;

    priv->fmt_ctx->flags             |= AVFMT_FLAG_NONBLOCK;
    priv->fmt_ctx->opaque             = ctx;
    priv->fmt_ctx->control_message_cb = adevsink_control_message;

    st = avformat_new_stream(priv->fmt_ctx, NULL);
    if (!st) {
        avformat_free_context(priv->fmt_ctx);
        priv->fmt_ctx = NULL;
        return AVERROR(ENOMEM);
    }

    ret = avformat_init_output(priv->fmt_ctx, options);
    if (ret < 0) {
        avformat_free_context(priv->fmt_ctx);
        priv->fmt_ctx = NULL;
        return ret;
    }

    return 0;
}

static void adevsink_uninit(AVFilterContext *ctx)
{
    ADevSinkPriv *priv = ctx->priv;

    adevsink_stop(ctx);

    avformat_free_context(priv->fmt_ctx);
    priv->fmt_ctx = NULL;
}

static int adevsink_output_packet(AVFilterContext *ctx, int status)
{
    ADevSinkPriv *priv = ctx->priv;
    AVPacket *pkt = priv->last_pkt;
    int ret = 0;

    if (!priv->started)
        return 0;

    while (pkt) {
        if (pkt->data) {
            ret = av_write_frame(priv->fmt_ctx, pkt);
            if (ret == 0)
                av_packet_unref(pkt);
            else
                break;
        }

        if (priv->enc_ctx) {
            ret = avcodec_receive_packet(priv->enc_ctx, pkt);
            if (ret < 0) {
                if (ret == AVERROR(EAGAIN))
                    ret = 0;
                break;
            }
        } else {
            break;
        }
    }

    if (ret == AVERROR_EOF || status) {
        // ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
        adevsink_stop(ctx);
    }

    return ret;
}

static int adevsink_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    ADevSinkPriv *priv = ctx->priv;
    AVPacket *pkt = NULL;
    int status = 0;
    int ret;

    if (priv->enc_ctx) {
        ret = avcodec_send_frame(priv->enc_ctx, frame);
        if (ret < 0)
            return ret;
    } else {
        pkt = frame ? (AVPacket *)frame->data[0] : NULL;
        ret = av_write_frame(priv->fmt_ctx, pkt);
        if (ret < 0) {
            ret = av_packet_ref(priv->last_pkt, pkt);
            if (ret < 0)
                av_log(ctx, AV_LOG_ERROR, "Failed to ref packets!\n");
        }

        status = !frame ? AVERROR_EOF : 0;
    }

    return adevsink_output_packet(ctx, status);
}

static int adevsink_activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    ADevSinkPriv *priv = ctx->priv;
    AVFrame *frame;
    int64_t pts;
    int ret;

    ret = adevsink_output_packet(ctx, 0);
    if (ret < 0)
        return ret;

    if (ff_inlink_check_available_frame(inlink)) {
        ret = adevsink_start(ctx);
        if (ret < 0) {
            // if (ret == AVERROR_EOF)
            //     ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
            return ret;
        }

        if (priv->enc_ctx && priv->enc_ctx->frame_size)
            ret = ff_inlink_consume_samples(inlink, priv->enc_ctx->frame_size, priv->enc_ctx->frame_size, &frame);
        else
            ret = ff_inlink_consume_frame(inlink, &frame);
        if (ret < 0)
            return ret;
        else if (ret > 0) {
            if (frame->nb_samples <= 0) {
                av_frame_free(&frame);
                return adevsink_send_frame(ctx, NULL);
            }
            else {
                ret = adevsink_send_frame(ctx, frame);
                av_frame_free(&frame);
                if (ret >= 0)
                    ff_filter_set_ready(ctx, 100);
                return ret;
            }
        }
    }

    ff_inlink_request_frame(inlink);

    return ret;
}

static int adevsink_query_formats(AVFilterContext *ctx)
{
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterLink *link = ctx->inputs[0];
    AVFilterFormats *formats = NULL;
    ADevSinkPriv *priv = ctx->priv;
    AVOptionRanges *ranges = NULL;
    AVChannelLayout layout;
    bool codec = false;
    int ret, i;

    if (priv->sample_fmt != AV_SAMPLE_FMT_NONE) {
        ret = ff_add_format(&formats, priv->sample_fmt);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges2(&ranges, priv->fmt_ctx->priv_data, priv->fmt_ctx, "sample_fmts", AV_OPT_MULTI_COMPONENT_RANGE);
        if (ret >= 0) {
            for (i = 0; i < ranges->nb_ranges; i++) {
                ret = ff_add_format(&formats, ranges->range[i]->value_min);
                if (ret < 0)
                    goto out;
            }
            av_opt_freep_ranges(&ranges);
        } else {
            formats = ff_all_formats(AVMEDIA_TYPE_AUDIO);
        }
    }

    ret = ff_set_common_formats(ctx, formats);
    if (ret < 0)
        goto out;

    formats = NULL;
    if (priv->sample_rate) {
        ret = ff_add_format(&formats, priv->sample_rate);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges2(&ranges, priv->fmt_ctx->priv_data, priv->fmt_ctx, "sample_rates", AV_OPT_MULTI_COMPONENT_RANGE);
        if (ret >= 0) {
            for (i = 0; i < ranges->nb_ranges; i++) {
                ret = ff_add_format(&formats, ranges->range[i]->value_min);
                if (ret < 0)
                    goto out;
            }
            av_opt_freep_ranges(&ranges);
        }
    }

    ret = ff_set_common_samplerates(ctx, formats);
    if (ret < 0)
        goto out;

    if (priv->ch_layout.nb_channels) {
        ret = ff_add_channel_layout(&layouts, &priv->ch_layout);
        if (ret < 0)
            goto out;
    } else {
        ret = av_opt_query_ranges2(&ranges, priv->fmt_ctx->priv_data, priv->fmt_ctx, "channels", AV_OPT_MULTI_COMPONENT_RANGE);
        if (ret >= 0) {
            int n;

            for (n = 0; n < ranges->nb_ranges; n++) {
                if (ranges->range[n]->is_range) {
                    for (i = ranges->range[0]->value_min; i <= ranges->range[0]->value_max; i++) {
                        av_channel_layout_default(&layout, i);
                        ret = ff_add_channel_layout(&layouts, &layout);
                        if (ret < 0)
                            goto out;
                    }
                } else {
                    i = ranges->range[n]->value_min;
                    av_channel_layout_default(&layout, i);
                    ret = ff_add_channel_layout(&layouts, &layout);
                    if (ret < 0)
                        goto out;
                }
            }

            av_opt_freep_ranges(&ranges);
        }
    }

    ret = ff_set_common_channel_layouts(ctx, layouts);
    if (ret < 0)
        goto out;

    ret = 0;

out:
    av_opt_freep_ranges(&ranges);
    return ret;
}

static int adevsink_process_command(AVFilterContext *ctx,
                                    const char *cmd, const char *args,
                                    char *res, int res_len, int flags)
{
    ADevSinkPriv *priv = ctx->priv;

    if (!strcmp(cmd, "start")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_START,
                                    res, res_len);
    } else if (!strcmp(cmd, "stop")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_STOP,
                                    res, res_len);
    } else if (!strcmp(cmd, "play")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                AV_APP_TO_DEV_PLAY,
                                res, res_len);
    }  else if (!strcmp(cmd, "pause")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                AV_APP_TO_DEV_PAUSE,
                                res, res_len);
    } else if (!strcmp(cmd, "volume")) {

        double volume;
        int ret;

        ret = av_expr_parse_and_eval(&volume, args, NULL, NULL,
                                     NULL, NULL, NULL, NULL,
                                     NULL, 0, NULL);
        if (ret < 0)
            return ret;

        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_SET_VOLUME,
                                    &volume, sizeof(double));
    } else if (!strcmp(cmd, "get_volume")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_GET_VOLUME,
                                    res, res_len);
    } else if (!strcmp(cmd, "get_position")) {
        return avdevice_app_to_dev_control_message(priv->fmt_ctx,
                                    AV_APP_TO_DEV_GET_POSITION,
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
    } else if (!strcmp(cmd, "flush")) {
        if (priv->last_pkt)
            av_packet_unref(priv->last_pkt);

        return avdevice_app_to_dev_control_message(
                                    priv->fmt_ctx,
                                    AV_APP_TO_DEV_FLUSH,
                                    res, res_len);
    } else if (!strcmp(cmd, "drain")) {
        return avdevice_app_to_dev_control_message(
                                    priv->fmt_ctx,
                                    AV_APP_TO_DEV_DRAIN,
                                    res, res_len);
    } else if (!strcmp(cmd, "dump")) {
        return avdevice_app_to_dev_control_message(
                                    priv->fmt_ctx,
                                    AV_APP_TO_DEV_DUMP,
                                    res, res_len);
    } else if (!strcmp(cmd, "get_timestamp")) {
        int64_t *ts  = ((int64_t **)res)[0];
        int64_t *lat = ((int64_t **)res)[1];

        int ret = av_get_output_timestamp(priv->fmt_ctx, 0, ts, lat);
        if (ret >= 0) {
            *ts  = av_rescale_q(*ts,  ctx->inputs[0]->time_base, AV_TIME_BASE_Q);
            *lat = av_rescale_q(*lat, ctx->inputs[0]->time_base, AV_TIME_BASE_Q);
        }

        return ret;
    } else {
        return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    }
}

static int adevsink_forward_command(AVFilterContext *ctx, int pad_idx, const char* target, const char *cmd,
                                    const char *arg, char *res, int res_len, int flags)
{
    return adevsink_process_command(ctx, cmd, arg, res, res_len, flags);
}

static const struct AVClass *adevsink_child_class_iterate(void **iter)
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

static void *adevsink_child_next(void *obj, void *prev)
{
    ADevSinkPriv *priv = obj;

    if (!prev)
        return priv->fmt_ctx;
    else if (prev == priv->fmt_ctx)
        return priv->enc_ctx;
    else
        return NULL;
}

#define OFFSET(x) offsetof(ADevSinkPriv, x)
#define FLAGS  AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define FLAGSR FLAGS|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption adevsink_options[] = {
    { "format",      "", OFFSET(format),      AV_OPT_TYPE_STRING,     .flags = FLAGS },
    { "devname",     "", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = FLAGS },
    { "sample_fmt",  "", OFFSET(sample_fmt),  AV_OPT_TYPE_SAMPLE_FMT, {.i64=AV_SAMPLE_FMT_NONE}, -1, INT_MAX, FLAGSR },
    { "sample_rate", "", OFFSET(sample_rate), AV_OPT_TYPE_INT,        {.i64 = 0},                 0, INT_MAX, FLAGSR },
    { "ch_layout",   "", OFFSET(ch_layout),   AV_OPT_TYPE_CHLAYOUT,   {.str = NULL},              0, 0,       FLAGSR },
    { "is_activate", "", OFFSET(started),     AV_OPT_TYPE_BOOL,       {.i64 = 0},                 0, 1,       FLAGSR },
    { NULL },
};

static const AVClass adevsink_class = {
    .class_name          = "adevsink_class",
    .item_name           = av_default_item_name,
    .option              = adevsink_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = adevsink_child_next,
    .child_class_iterate = adevsink_child_class_iterate,
};

static const AVFilterPad adevsink_inputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
};

const AVFilter ff_asink_adevsink = {
    .name            = "adevsink",
    .description     = NULL_IF_CONFIG_SMALL("Audio adevice sink"),
    .priv_class      = &adevsink_class,
    .priv_size       = sizeof(ADevSinkPriv),
    .init            = adevsink_init_dict,
    .uninit          = adevsink_uninit,
    .activate        = adevsink_activate,
    FILTER_INPUTS(adevsink_inputs),
    FILTER_QUERY_FUNC(adevsink_query_formats),
    .process_command = adevsink_process_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
