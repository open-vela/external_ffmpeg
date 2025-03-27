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
 * tinycompress src
 */

#include <libavcodec/avcodec.h>
#include <libavutil/avstring.h>
#include <libavformat/internal.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>

#include "avfilter.h"
#include "avfilter_internal.h"
#include "aresample.h"
#include "filters.h"
#include "formats.h"
#include "libavutil/mem.h"

#include <tinycompress/tinycompress.h>
#include <sound/compress_params.h>
#include <poll.h>

typedef struct TinyCompressContext {
    const AVClass *class;
    struct compress *compress;
    AVCodecContext *dec_ctx;
    AResampleContext resampler;

    enum AVSampleFormat sample_fmt;
    AVChannelLayout ch_layout;
    enum AVCodecID codec_id;
    char *devname;
    int sample_rate;
    int fragment_size;
    int fragments;

    char *map_str;
    int *map;
    int nb_outputs;
    int64_t next_pts;
    AVPacket *pkt;
} TinyCompressContext;

static int tinycompr_fmt_to_avcodec(int audio_fmt)
{
    switch (audio_fmt) {
        case AUDIO_FMT_MP3:
            return AV_CODEC_ID_MP3;
        case AUDIO_FMT_AC3:
            return AV_CODEC_ID_AC3;
        case AUDIO_FMT_WMA:
            return AV_CODEC_ID_WMAV2;
        case AUDIO_FMT_DTS:
            return AV_CODEC_ID_DTS;
        case AUDIO_FMT_OGG_VORBIS:
            return AV_CODEC_ID_VORBIS;
        case AUDIO_FMT_FLAC:
            return AV_CODEC_ID_FLAC;
        case AUDIO_FMT_AMR:
            return AV_CODEC_ID_AMR_NB; // Assuming AMR_NB as default
        case AUDIO_FMT_OPUS:
            return AV_CODEC_ID_OPUS;
        case AUDIO_FMT_AAC:
            return AV_CODEC_ID_AAC;
    }

    return AV_CODEC_ID_PCM_S16LE;
}

static int tinycompr_subfmt_to_smpfmt(int subfmt)
{
    switch (subfmt) {
        case AUDIO_SUBFMT_PCM_U8:
            return AV_SAMPLE_FMT_U8;
        case AUDIO_SUBFMT_PCM_S16_LE:
        case AUDIO_SUBFMT_PCM_S16_BE:
            return AV_SAMPLE_FMT_S16;
        case AUDIO_SUBFMT_PCM_S32_LE:
        case AUDIO_SUBFMT_PCM_S32_BE:
            return AV_SAMPLE_FMT_S32;
    }

    return AV_SAMPLE_FMT_NONE;
}

static inline void tinycomprsrc_force_request(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    for (int i = 0; i < ctx->nb_outputs; i++) {
        if (s->map && s->map[i] == 0)
            continue;
        FilterLinkInternal *li = ff_link_internal(ctx->outputs[i]);
        li->frame_wanted_out = 1;
    }
    ff_filter_set_ready(ctx, 100);
}

static int query_formats(const AVFilterContext *ctx,
                        AVFilterFormatsConfig **cfg_in,
                        AVFilterFormatsConfig **cfg_out)
{
    int sample_rates[] = { 44100, -1 };
    AVChannelLayout layout[] = { AV_CHANNEL_LAYOUT_STEREO, { 0 } };
    enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int ret;

    ret = ff_set_common_formats_from_list2(ctx, cfg_in, cfg_out, sample_fmts);
    if (ret < 0)
        return ret;

    ret = ff_set_common_channel_layouts_from_list2(ctx, cfg_in, cfg_out, layout);
    if (ret < 0)
        return ret;

    return ff_set_common_samplerates_from_list2(ctx, cfg_in, cfg_out, sample_rates);
}

static int tinycomprsrc_receive_frame(AVFilterContext *ctx, AVFrame **frame) {
    TinyCompressContext *s = ctx->priv;
    AVPacket *pkt = s->pkt;
    AVFrame *out;
    int ret;

    out = av_frame_alloc();
    if (!out)
        return AVERROR(ENOMEM);

    while (1) {
        ret = avcodec_receive_frame(s->dec_ctx, out);
        if (ret >= 0)
            break;
        else if (ret != AVERROR(EAGAIN))
            goto error;

        ret = compress_read(s->compress, pkt->data, s->fragment_size);
        if (ret < 0)
            goto error;

        pkt->size = ret;
        pkt->pts = s->next_pts;
        s->next_pts += ret / av_get_bytes_per_sample(s->sample_fmt);

        ret = avcodec_send_packet(s->dec_ctx, pkt);

        if (ret < 0)
            goto error;
    }

    av_channel_layout_copy(&out->ch_layout, &s->ch_layout);
    *frame = out;
    return 0;
error:
    av_frame_free(&out);
    return ret;
}

static int tinycomprsrc_open(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    struct compr_config config = { 0 };
    const AVCodec *dec;
    int ret;

    if (s->dec_ctx)
        return 0;

    s->compress = compress_open_by_name(s->devname, COMPRESS_OUT, &config);
    if (!s->compress || !is_compress_ready(s->compress)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to open device node: %s\n", s->devname);
        return AVERROR(EIO);
    }

    s->fragment_size = config.fragment_size;
    s->fragments = config.fragments;
    s->sample_fmt = tinycompr_subfmt_to_smpfmt(config.codec->format);
    s->sample_rate = config.codec->sample_rate;
    av_channel_layout_default(&s->ch_layout, config.codec->ch_in);
    s->codec_id = tinycompr_fmt_to_avcodec(config.codec->id);

    if (config.codec)
        free(config.codec);

    compress_nonblock(s->compress, 1);

    dec = avcodec_find_decoder(s->codec_id);
    if (!dec) {
        ret = AVERROR(EINVAL);
        goto error;
    }

    s->dec_ctx = avcodec_alloc_context3(dec);
    if (!s->dec_ctx) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    s->dec_ctx->sample_rate = s->sample_rate;
    s->dec_ctx->ch_layout = s->ch_layout;
    s->dec_ctx->sample_fmt = s->sample_fmt;

    ret = avcodec_open2(s->dec_ctx, dec, NULL);
    if (ret < 0) {
        goto error;
    }

    ff_resample_init(&s->resampler);

    s->pkt = av_packet_alloc();
    if (!s->pkt)
        goto error;

    ret = av_new_packet(s->pkt, s->fragment_size);
    if (ret < 0) {
        av_packet_free(&s->pkt);
        goto error;
    }

    ret = compress_start(s->compress);
    if (ret < 0)
        goto error;

    return ret;

error:
    avcodec_free_context(&s->dec_ctx);
    if (s->compress) {
        compress_close(s->compress);
        s->compress = NULL;
    }
    av_packet_free(&s->pkt);
    s->dec_ctx = NULL;

    return ret;
}

static void tinycomprsrc_close(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    compress_stop(s->compress);
    avcodec_free_context(&s->dec_ctx);
    compress_close(s->compress);
    av_packet_free(&s->pkt);
    ff_resample_uninit(&s->resampler);
    s->compress = NULL;
    s->dec_ctx = NULL;
}

static int tinycomprsrc_check_outlink_status(AVFilterContext *ctx) {
    TinyCompressContext *s = ctx->priv;
    int need_close = 1;
    int ret;

    for (int i = 0; i < ctx->nb_outputs; i++) {
        if (ff_outlink_get_status(ctx->outputs[i]) != AVERROR_EOF) {
            need_close = 0;
            break;
        }
    }

    if (need_close && s->compress) {
        tinycomprsrc_close(ctx);
        return 0;
    }

    return 1;
}

static int activate(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    AVFrame *frame;
    int ret, i;

    ret = tinycomprsrc_check_outlink_status(ctx);
    if (!ret)
        return ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ff_outlink_frame_wanted(ctx->outputs[i]))
            break;
    }
    if (i == ctx->nb_outputs)
        return AVERROR(EAGAIN);

    ret = tinycomprsrc_receive_frame(ctx, &frame);
    if (ret < 0)
        goto out;

    for (i = 0; i < ctx->nb_outputs; i++) {
        AVFilterLink *link = ctx->outputs[i];
        AVFrame *resampled;
        if (s->map && s->map[i] == 0)
            continue;

        ret = ff_resample_frame(&s->resampler, link, frame, &resampled);
        if (ret <= 0) {
            resampled = av_frame_clone(frame);
            if (!resampled) {
                ret = AVERROR(ENOMEM);
                goto out;
            }
        }

        ret = ff_filter_frame(link, resampled);
        if (ret < 0)
            goto out;
    }

    av_frame_free(&frame);

    return ret;

out:
    if (ret < 0 && ret != AVERROR(EAGAIN))
        ff_filter_set_ready(ctx, 100);

    return ret;
}

static int config_props(AVFilterLink *link)
{
    return 0;
}

static av_cold int init(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    int ret;

    for (int i = 0; i < s->nb_outputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("output%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        pad.config_props = config_props;
        if ((ret = ff_append_outpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    if (s->map_str) {
        ret = avfilter_parse_mapping(s->map_str, &s->map, s->nb_outputs);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to parse mapping: %s\n", s->map_str);
            return ret;
        }
    }

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    av_freep(&s->map);
}

static int tinycomprsrc_process_command(AVFilterContext *ctx, const char *cmd, const char *arg,
                                    char *res, int res_len, int flags)
{
    TinyCompressContext *s = ctx->priv;
    int ret = 0;

    if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll_fd = (struct pollfd *)res;
        if (s->compress) {
            poll_fd[0].fd = compress_get_file_descriptor(s->compress);
            poll_fd[0].events = POLLIN;
            ret = 1;
        }
    } else if (!strcmp(cmd, "poll_available")) {
        struct pollfd *poll_fd = (struct pollfd *)res;
        if (poll_fd[0].fd == compress_get_file_descriptor(s->compress)) {
            ff_filter_set_ready(ctx, 100);
        }
        return 0;
    } else if (!strcmp(cmd, "link")) {
        ret = tinycomprsrc_open(ctx);
        tinycomprsrc_force_request(ctx);
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        for (int i = 0; i < ctx->nb_outputs; i++) {
            AVFilterLink *link = ctx->outputs[i];
            if (s->map && s->map[i] == 0)
                continue;
            ff_inlink_set_status(link, AVERROR_EOF);
        }
        s->next_pts = 0;
        tinycomprsrc_close(ctx);
        return 0;
    } else if (!strcmp(cmd, "map")) {
        int need_close = 1;
        ret = avfilter_parse_mapping(arg, &s->map, s->nb_outputs);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to parse mapping: %s ret:%d\n", arg, ret);
            return ret;
        }

        for (int i = 0; i < ctx->nb_outputs; i++) {
            AVFilterLink *link = ctx->outputs[i];
            if (s->map && s->map[i] == 0)
                ff_inlink_set_status(link, AVERROR_EOF);
            else if (s->map && s->map[i] == 1)
                need_close = 0;
        }

        ff_filter_set_ready(ctx, 100);
        if (need_close)
            tinycomprsrc_close(ctx);
        return ret;
    } else {
        ret = ff_filter_process_command(ctx, cmd, arg, res, res_len, flags);
    }

    return ret;
}

static const AVFilterPad tinycomprsrc_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

#define OFFSET(x) offsetof(TinyCompressContext, x)
#define A AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_FILTERING_PARAM
#define R A|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption tinycomprsrc_options[] = {
    { "devname",     "device name", OFFSET(devname),     AV_OPT_TYPE_STRING,     .flags = A },
    { "outputs",     "output link num", OFFSET(nb_outputs),  AV_OPT_TYPE_INT,        {.i64 = 1},                  0, INT_MAX, R },
    { "fragment_size", "fragment size", OFFSET(fragment_size), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, R },
    { "fragments",   "fragment num", OFFSET(fragments),   AV_OPT_TYPE_INT,        {.i64 = 0}, 0, INT_MAX, R },
    { "map",         "input indexes to remap to outputs", OFFSET(map_str),     AV_OPT_TYPE_STRING,     {.str = NULL},                    .flags=R },
    { "map_array",   "get map list", OFFSET(map),         AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { NULL },
};

AVFILTER_DEFINE_CLASS(tinycomprsrc);

const AVFilter ff_asrc_tinycomprsrc = {
    .name          = "tinycomprsrc",
    .description   = NULL_IF_CONFIG_SMALL("Read audio data using tinycompress."),
    .priv_size     = sizeof(TinyCompressContext),
    .priv_class    = &tinycomprsrc_class,
    FILTER_QUERY_FUNC2(query_formats),
    .activate      = activate,
    .init          = init,
    .uninit        = uninit,
    .process_command = tinycomprsrc_process_command,
    .outputs       = tinycomprsrc_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_POLL | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};