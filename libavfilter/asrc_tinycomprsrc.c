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
#include <libavformat/internal.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/eval.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include "libavutil/intreadwrite.h"

#include "alsa.h"
#include "avfilter.h"
#include "avfilter_internal.h"
#include "filters.h"
#include "formats.h"
#include "volume.h"
#include "mapping.h"

#include <nuttx/audio/audio.h>
#include <poll.h>
#include <sound/compress_params.h>
#include <tinycompress/tinycompress.h>

#define LC3_EXTRADATA_SIZE 6

enum CompSinkState {
    COMPSRC_STARTING = 1,
    COMPSRC_PAUSING = 2,
    COMPSRC_PAUSED = 3,
    COMPSRC_STARTED = 4,
    COMPSRC_STOPPED = 5
};

const char *state_str[] = {
    [COMPSRC_STARTING] = "COMPSRC_STARTING",
    [COMPSRC_PAUSING] = "COMPSRC_PAUSING",
    [COMPSRC_PAUSED] = "COMPSRC_PAUSED",
    [COMPSRC_STARTED] = "COMPSRC_STARTED",
    [COMPSRC_STOPPED] = "COMPSRC_STOPPED"
};

typedef struct TinyCompressContext {
    const AVClass *class;
    struct compress *compress;
    AVCodecContext *dec_ctx;

    enum AVSampleFormat sample_fmt;
    AVChannelLayout ch_layout;
    enum AVCodecID codec_id;
    char *devname;
    int sample_rate;
    int fragment_size;
    int fragments;
    int frame_us;

    char *map_str;
    int *map;
    int nb_outputs;
    enum PrecisionType precision;
    int64_t next_pts;
    AVPacket *pkt;

    enum CompSinkState state;
    bool start; /* Record the latest running state based on command*/
    bool unlinked;
    VolumeContext vol_ctx;
    double volume;
} TinyCompressContext;

static inline void tinycomprsrc_force_request(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    FilterLinkInternal *li;
    AVFilterLink *link;

    for (int i = 0; i < ctx->nb_outputs; i++) {
        if (s->map[i] == 0)
            continue;

        link = ctx->outputs[i];
        li = ff_link_internal(link);
        li->frame_wanted_out = 1;
    }

    ff_filter_set_ready(ctx, 100);
}

static int tinycomprsrc_receive_frame(AVFilterContext *ctx, AVFrame **frame) {
    TinyCompressContext *s = ctx->priv;
    AVPacket *pkt = s->pkt;
    AVFrame *out;
    int bytes_per_sample, ret;

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
        if (ret > 0 && ret != s->fragment_size)
            av_log(ctx, AV_LOG_ERROR, "Not read enough data fragment_size:%d ret:%d\n", s->fragment_size, ret);
        else if (ret < 0) {
            /* If paused and no start cmd wait, then set link EOF. */
            if (!s->start && (s->state == COMPSRC_PAUSING || s->state == COMPSRC_PAUSED)) {
                av_log(ctx, AV_LOG_INFO, "%s sync EOF.", ctx->name);
                for (int i = 0; i < ctx->nb_outputs; i++) {
                    AVFilterLink *link = ctx->outputs[i];

                    if (s->map[i] == 0)
                        continue;

                    ff_outlink_set_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
                }
            }
            goto error;
        }

        pkt->size = ret;
        pkt->pts = s->next_pts;
        pkt->time_base =  (AVRational) { 1, s->sample_rate };
        pkt->duration = (s->frame_us * s->sample_rate) / 1000000;

        bytes_per_sample = av_get_bytes_per_sample(s->sample_fmt);
        if (bytes_per_sample <= 0) {
            av_log(ctx, AV_LOG_ERROR, "Invalid sample format: %d\n", s->sample_fmt);
            goto error;
        }

        s->next_pts += ret / bytes_per_sample;

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

static int tinycomprsrc_pause(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    int ret;

    if (!s->compress || s->state != COMPSRC_STARTED)
        return 0;

    ret = compress_pause(s->compress);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "%s pause fail.\n", ctx->name);
        return ret;
    }

    s->state = COMPSRC_PAUSING;
    av_log(ctx, AV_LOG_INFO, "%s pause.\n", ctx->name);
    return ret;
}

static int tinycomprsrc_resume(AVFilterContext *ctx) {
    TinyCompressContext *s = ctx->priv;
    int ret;

    if (!s->compress || s->state != COMPSRC_PAUSED)
        return 0;

    ret = compress_resume(s->compress);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "%s resume fail.\n", ctx->name);
        return ret;
    }

    s->state = COMPSRC_STARTING;
    av_log(ctx, AV_LOG_INFO, "%s resume.\n", ctx->name);
    return 0;
}

static void tinycomprsrc_close(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    if (!s->compress)
        return;

    av_log(ctx, AV_LOG_INFO, "%s close.\n", ctx->name);

    volume_uninit(&s->vol_ctx);
    avcodec_free_context(&s->dec_ctx);
    compress_close(s->compress);
    av_packet_free(&s->pkt);
    s->state = COMPSRC_STOPPED;
    s->next_pts = 0L;
    s->compress = NULL;
    s->dec_ctx = NULL;
    s->sample_fmt = AV_SAMPLE_FMT_NONE;
    s->unlinked = false;
}

static void tinycomprsrc_control_callback(FAR void* cookie, int event, const FAR void* extra)
{
    AVFilterContext *ctx = (AVFilterContext *)cookie;
    TinyCompressContext *s = ctx->priv;
    const char *event_str[] = {
        [AUDIO_MSG_START] = "STARTED",
        [AUDIO_MSG_PAUSE] = "PAUSED",
        [AUDIO_MSG_RESUME] = "RESUMED",
        [AUDIO_MSG_COMPLETE] = "COMPLETED"
    };

    av_log(ctx, AV_LOG_INFO, "tinycomprsrc event:%s state:%s\n", event_str[event], state_str[s->state]);

    switch (event) {
        case AUDIO_MSG_START:
        case AUDIO_MSG_RESUME:
            if (s->state == COMPSRC_STARTING)
                s->state = COMPSRC_STARTED;
            break;
        case AUDIO_MSG_PAUSE:
            if (s->state == COMPSRC_PAUSING)
                s->state = COMPSRC_PAUSED;
            break;

        /* Device won't fail*/
        // case AUDIO_MSG_IOERR:

        default:
            return;
    }
}

static int tinycomprsrc_codec_to_options(AVFilterContext *ctx, struct snd_codec *codec)
{
    TinyCompressContext *priv = ctx->priv;

    if (!codec)
        return AVERROR(EINVAL);

    switch (priv->codec_id) {
        case AV_CODEC_ID_LC3: {
            bool hr_mode = false;
            priv->frame_us = codec->options.lc3_d.frame_duration * 1000;
            hr_mode |= priv->sample_rate > 48000;
            hr_mode &= priv->sample_rate >= 48000;

            priv->dec_ctx->extradata = av_mallocz(LC3_EXTRADATA_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!priv->dec_ctx->extradata)
                return AVERROR(ENOMEM);

            AV_WL16(priv->dec_ctx->extradata + 0, priv->frame_us / 10);
            AV_WL16(priv->dec_ctx->extradata + 2, 0);
            AV_WL16(priv->dec_ctx->extradata + 4, hr_mode);
            priv->dec_ctx->extradata_size = LC3_EXTRADATA_SIZE;
            break;
        }

        default:
            av_log(ctx, AV_LOG_ERROR, "Unsupported codec: %"PRIu32"\n", codec->id);
            break;
    }

    return 0;
}

static int tinycomprsrc_open(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    struct compr_config config = { 0 };
    struct snd_codec codec = { 0 };
    const AVCodec *dec;
    char devname[64];
    int ret = -EINVAL;

    if (s->dec_ctx || s->compress)
        return 0;

    snprintf(devname, sizeof(devname), "/dev/audio/%s", s->devname);
    s->compress = compress_open_by_name(devname, COMPRESS_OUT, NULL);
    if (!s->compress || !is_compress_ready(s->compress)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to open device node: %s\n", s->devname);
        ret = AVERROR(EIO);
        goto error;
    }

    config.codec = &codec;
    ret = compress_get_current_config(s->compress, &config);
    if (ret < 0)
        goto error;

    ret = compress_set_current_config(s->compress, &config);
    if (ret < 0)
        goto error;

    s->fragment_size = config.fragment_size;
    s->fragments = config.fragments;

    compress_nonblock(s->compress, 1);
    compress_set_event_callback(s->compress, tinycomprsrc_control_callback, ctx);

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
    s->dec_ctx->request_sample_fmt = s->sample_fmt;
    ret = tinycomprsrc_codec_to_options(ctx, config.codec);
    if (ret < 0)
        goto error;

    ret = avcodec_open2(s->dec_ctx, dec, NULL);
    if (ret < 0) {
        goto error;
    }

    s->pkt = av_packet_alloc();
    if (!s->pkt) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    ret = av_new_packet(s->pkt, s->fragment_size);
    if (ret < 0)
        goto error;

    ret = compress_start(s->compress);
    if (ret < 0)
        goto error;

    s->state = COMPSRC_STARTING;
    av_log(ctx, AV_LOG_INFO, "%s start.\n", ctx->name);

    ret = volume_init(&s->vol_ctx, s->sample_fmt, s->precision);
    if (ret < 0)
        goto error;

    volume_set(&s->vol_ctx, s->volume);
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

static int activate(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    AVFrame *frame = NULL;
    int ret, i;

    /* if pausing, supposed to read all left data from device */
    if (s->state < COMPSRC_PAUSING) {
        av_log(ctx, AV_LOG_WARNING, "%s busy state:%s\n", ctx->name, state_str[s->state]);
        return AVERROR(EAGAIN);
    }

    if (s->unlinked) {
        tinycomprsrc_close(ctx);
        return 0;
    } else {
        if (s->state == COMPSRC_PAUSED && s->start) {
            return tinycomprsrc_resume(ctx);
        } else if (s->state == COMPSRC_STARTED && !s->start) {
            return tinycomprsrc_pause(ctx);
        }
    }

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ff_outlink_frame_wanted(ctx->outputs[i]))
            break;
    }
    if (i == ctx->nb_outputs)
        return FFERROR_NOT_READY;

    ret = tinycomprsrc_open(ctx);
    if (ret < 0)
        return ret;

    ret = tinycomprsrc_receive_frame(ctx, &frame);
    if (ret < 0)
        goto out;

    volume_scale(&s->vol_ctx, frame);

    for (i = 0; i < ctx->nb_outputs; i++) {
        AVFrame *iframe = NULL;

        if (s->map[i] == 0)
            continue;

        iframe = av_frame_clone(frame);
        if (!iframe) {
            ret = AVERROR(ENOMEM);
            goto out;
        }

        ret = ff_filter_frame(ctx->outputs[i], iframe);
        if (ret < 0)
            goto out;
    }

out:
    av_frame_free(&frame);
    if (ret == AVERROR(EAGAIN))
        return 0;
    return ret;
}

static int config_props(AVFilterLink *link)
{
    AVFilterContext *ctx = link->src;
    TinyCompressContext *s = ctx->priv;

    s->sample_fmt = link->format;

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

    s->state = COMPSRC_STOPPED;
    s->volume = 1.0f;

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    TinyCompressContext *s = ctx->priv;
    tinycomprsrc_close(ctx);
    av_freep(&s->map);
}

static int tinycomprsrc_query_cap(TinyCompressContext *s, const char *format, int *out_value)
{
    AVOptionRanges* ranges = NULL;
    AVOptionRange* range = NULL;
    int ret;

    ret = alsa_query_caps(&ranges, s->devname, format, false);
    if (ret > 0) {
        *out_value = ranges->range[0]->value_min;
        av_opt_freep_ranges(&ranges);
    }

    return ret;
}

static int tinycomprsrc_query_formats(TinyCompressContext *s)
{
    int codec_id;
    int ret;
    int tmp_fmt;

    ret = tinycomprsrc_query_cap(s, "codec", &codec_id);
    if (ret < 0)
        return ret;

    s->codec_id = codec_id;

    ret = tinycomprsrc_query_cap(s, "sample_fmts", &tmp_fmt);
    if (ret < 0)
        return ret;
    s->sample_fmt = tmp_fmt;

    ret = tinycomprsrc_query_cap(s, "sample_rates", &s->sample_rate);
    if (ret < 0)
        return ret;

    ret = tinycomprsrc_query_cap(s, "channels", &s->ch_layout.nb_channels);
    if (ret < 0)
        return ret;

    av_channel_layout_default(&s->ch_layout, s->ch_layout.nb_channels);

    return 0;
}

static int tinycomprsrc_get_parameter(AVFilterContext *ctx, const char *key, char *value, int len)
{
    TinyCompressContext *s = ctx->priv;
    int ret;

    if (!strcmp(key, "volume")) {
        snprintf(value, len, "vol:%f", s->vol_ctx.volume);

        av_log(ctx, AV_LOG_DEBUG, "get_parameter: %s = %.2f\n", key, s->vol_ctx.volume);
        return 0;
    }

    av_log(ctx, AV_LOG_ERROR, "get_parameter [%s] not found.\n", key);
    return AVERROR(EINVAL);
}

static int tinycomprsrc_set_parameter(AVFilterContext *ctx, const char *args)
{
    TinyCompressContext *s = ctx->priv;
    char *key = NULL, *value = NULL;
    const char *p = args;
    int ret = 0;

    av_log(ctx, AV_LOG_INFO, "Parsing args: %s\n", args);

    while (*p) {
        ret = av_opt_get_key_value(&p, "=", ":", 0, &key, &value);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Unable to parse '%s': %s\n", p, av_err2str(ret));
            break;
        }

        if (*p)
            p++;

        av_log(ctx, AV_LOG_INFO, "Parsed Key: %s, Value: %s\n", key, value);

        if (!strcmp(key, "volume")) {
            double volume;

            ret = av_expr_parse_and_eval(&volume, value, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR,
                    "Error when parsing %s volume expression '%s'\n", ctx->name, value);
                goto end;
            }

            volume_set(&s->vol_ctx, volume);
            s->volume = volume;

            av_log(ctx, AV_LOG_INFO, "set_parameter: %s = %.2f\n", key, s->vol_ctx.volume);
        } else
            av_log(ctx, AV_LOG_ERROR, "Unknown parameter: %s\n", key);

end:
        av_freep(&key);
        av_freep(&value);
    }

    return ret;
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const enum AVSampleFormat *sample_fmts = NULL;
    TinyCompressContext *priv = ctx->priv;
    AVFilterFormats *formats = NULL;
    AVCodecContext *dec_ctx = NULL;
    int ret = 0, fmt = 0, count;
    const AVCodec *dec;

    ret = tinycomprsrc_query_formats(priv);
    if (ret < 0)
        return ret;

    dec = avcodec_find_decoder(priv->codec_id);
    if (!dec)
        return AVERROR_DECODER_NOT_FOUND;

    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        return AVERROR(ENOMEM);

    ret = avcodec_get_supported_config(dec_ctx, NULL,
                                       AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                       (const void **)&sample_fmts, &count);
    if (ret >= 0 && count > 0 && sample_fmts != NULL) {
        formats = ff_make_sample_format_list(sample_fmts);
        fmt = !!formats;
    }

    for (int i = 0; i < ctx->nb_outputs; i++) {
        const AVChannelLayout layout_list[] = { priv->ch_layout, { 0 } };
        ff_formats_unref(&cfg_out[i]->formats);
        if (fmt)
            ret = ff_formats_ref(formats, &cfg_out[i]->formats);
        else
            ret = ff_formats_ref(ff_make_format_list((const int[]){ priv->sample_fmt, -1 }), &cfg_out[i]->formats);

        if (ret < 0)
            goto out;

        ff_formats_unref(&cfg_out[i]->samplerates);
        ret = ff_formats_ref(ff_make_format_list((const int[]){ priv->sample_rate, -1 }), &cfg_out[i]->samplerates);
        if (ret < 0)
            goto out;

        ff_channel_layouts_unref(&cfg_out[i]->channel_layouts);
        ret = ff_channel_layouts_ref(ff_make_channel_layout_list(layout_list), &cfg_out[i]->channel_layouts);
    }

out:
    ff_formats_unref(&formats);
    avcodec_free_context(&dec_ctx);
    return ret;
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
            return 1;
        }
    } else if (!strcmp(cmd, "poll_available")) {
        compress_poll_available(s->compress);
        ff_filter_set_ready(ctx, 100);
        return 0;
    } else if (!strcmp(cmd, "link")) {
        s->start = true;
        if (s->compress) {
            if (s->state == COMPSRC_PAUSED)
                tinycomprsrc_resume(ctx);
        }

        tinycomprsrc_force_request(ctx);
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        for (int i = 0; i < ctx->nb_outputs; i++) {
            AVFilterLink *link = ctx->outputs[i];

            if (s->map[i] == 0)
                continue;
            ff_outlink_set_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
        }

        av_log(ctx, AV_LOG_INFO, "%s unlink.\n", ctx->name);
        if (s->state < COMPSRC_PAUSED)
            s->unlinked = true;
        else
            tinycomprsrc_close(ctx);
        return 0;
    } else if (!strcmp(cmd, "map")) {
        ret = avfilter_parse_mapping(arg, &s->map, s->nb_outputs);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to parse mapping: %s ret:%d\n", arg, ret);
            return ret;
        }

        for (int i = 0; i < ctx->nb_outputs; i++) {
            AVFilterLink *link = ctx->outputs[i];

            if (s->map[i] == 0)
                ff_outlink_set_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
        }

        return ret;
    } else if (!strcmp(cmd, "pause")) {
        s->start = false;
        ret = tinycomprsrc_pause(ctx);
    } else if (!strcmp(cmd, "get_parameter")) {
        if (!arg || res_len <= 0)
            return AVERROR(EINVAL);

        return tinycomprsrc_get_parameter(ctx, arg, res, res_len);
    } else if (!strcmp(cmd, "set_parameter")) {
        if (!arg)
            return AVERROR(EINVAL);

        return tinycomprsrc_set_parameter(ctx, arg);
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
    { "devname",       "device name",                       OFFSET(devname),       AV_OPT_TYPE_STRING, .flags = A },
    { "outputs",       "output link num",                   OFFSET(nb_outputs),    AV_OPT_TYPE_INT,    {.i64 = 1}, 0, INT_MAX, R },
    { "map",           "input indexes to remap to outputs", OFFSET(map_str),       AV_OPT_TYPE_STRING, {.str = NULL},   .flags=R },
    { "map_array",     "get map list",                      OFFSET(map),           AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY, .max = INT_MAX, .flags = A|R },
    { "precision", "select mathematical precision",
            OFFSET(precision), AV_OPT_TYPE_INT, { .i64 = PRECISION_FIXED }, PRECISION_FIXED, PRECISION_DOUBLE, A, "precision" },
        { "fixed",  "select 8-bit fixed-point",     0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_FIXED  }, INT_MIN, INT_MAX, A, "precision" },
        { "float",  "select 32-bit floating-point", 0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_FLOAT  }, INT_MIN, INT_MAX, A, "precision" },
        { "double", "select 64-bit floating-point", 0, AV_OPT_TYPE_CONST, { .i64 = PRECISION_DOUBLE }, INT_MIN, INT_MAX, A, "precision" },
    { NULL },
};

AVFILTER_DEFINE_CLASS(tinycomprsrc);

const AVFilter ff_asrc_tinycomprsrc = {
    .name          = "tinycomprsrc",
    .description   = NULL_IF_CONFIG_SMALL("Read audio data using tinycompress."),
    .priv_size     = sizeof(TinyCompressContext),
    .priv_class    = &tinycomprsrc_class,
    .activate      = activate,
    .init          = init,
    .uninit        = uninit,
    FILTER_QUERY_FUNC2(query_formats),
    .process_command = tinycomprsrc_process_command,
    .outputs       = tinycomprsrc_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_POLL | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};