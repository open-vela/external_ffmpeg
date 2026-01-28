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
 * audio tinycompress sink
 */

#include <poll.h>
#include <nuttx/audio/audio.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
#include <sound/compress_params.h>
#include <tinycompress/tinycompress.h>

#include "alsa.h"
#include "amix.h"
#include "filters.h"
#include "avfilter.h"
#include "formats.h"
#include "avfilter_internal.h"

#define TINYCOMPRSINK_SILENCE_FRAME_DURATION 20

enum InputState
{
    INPUT_PAUSED = 1,
    INPUT_STARTED = 2
};

enum CompSinkState {
    COMPSINK_STARTING = 1,
    COMPSINK_PAUSING = 2,
    COMPSINK_RESUMING = 3,
    COMPSINK_PAUSED = 4,
    COMPSINK_RESUMED = 5,
    COMPSINK_STARTED = 6,
    COMPSINK_STOPPED = 7
};

const char* comp_sink_state_str[] = {
    [COMPSINK_STARTING] = "COMPSINK_STARTING",
    [COMPSINK_PAUSING]  = "COMPSINK_PAUSING",
    [COMPSINK_RESUMING] = "COMPSINK_RESUMING",
    [COMPSINK_PAUSED]   = "COMPSINK_PAUSED",
    [COMPSINK_RESUMED]  = "COMPSINK_RESUMED",
    [COMPSINK_STARTED]  = "COMPSINK_STARTED",
    [COMPSINK_STOPPED]  = "COMPSINK_STOPPED"
};

typedef struct CompSinkPriv {
    const AVClass *class;
    AVCodecContext *enc_ctx;
    int nb_inputs;
    char *devname;
    int sample_fmt;
    uint32_t sample_rate;
    AVChannelLayout ch_layout;
    enum AVCodecID codec_id;
    AVPacket *last_pkt;
    AMixContext *mix;
    int frame_count;              /**< number of silent frames before pause */
    int timeout;                  /**< time tolerance for silence frame before pause */
    enum CompSinkState state;
    enum InputState *input_state; /**< start state of each input */
    bool unlinked;
    FAR struct compress *compress;
} CompSinkPriv;

static void tinycomprsink_codec_to_options(AVFilterContext *ctx, struct snd_codec *codec, AVDictionary **options)
{
    CompSinkPriv *priv = ctx->priv;
    char buffer[128];

    if (!codec)
        return;

    switch (priv->codec_id) {
        case AV_CODEC_ID_SBC:
            snprintf(buffer, sizeof(buffer),
                     "channel_mode=%d:blocks=%d:subbands=%d:alloc_method=%d:bitpool=%d",
                     codec->ch_mode,
                     codec->options.sbc.blocks,
                     codec->options.sbc.subbands,
                     codec->options.sbc.alloc_method,
                     codec->options.sbc.bitpool);
            av_log(ctx, AV_LOG_INFO, "%s\n", buffer);
            av_dict_set(options, "sbc_param", buffer, 0);
            break;

        case AV_CODEC_ID_AAC:
            av_dict_set_int(options, "profile", codec->profile, 0);
            av_dict_set_int(options, "vbr", codec->rate_control, 0);
            av_dict_set_int(options, "latm", 1, 0);
            av_dict_set_int(options, "peak", 1, 0);
            break;

        case AV_CODEC_ID_LC3:
            snprintf(buffer, sizeof(buffer), "%.4f", codec->options.lc3.frame_duration);
            av_dict_set(options, "frame_duration", buffer, 0);
            break;

        case AV_CODEC_ID_SPEEX:
            av_dict_set_int(options, "cbr_quality", codec->options.spx.cbr_quality, 0);
            av_dict_set_int(options, "compression_level", codec->options.spx.compression_level, 0);
            av_dict_set_int(options, "frames_per_packet", codec->options.spx.frames_per_packet, 0);
            break;

        default:
            av_log(ctx, AV_LOG_ERROR, "Unsupported codec: %d\n", codec->id);
            break;
    }
}

static int tinycomprsink_open_encoder(AVFilterContext *ctx, AVDictionary **options)
{
    CompSinkPriv *priv = ctx->priv;
    const AVCodec *enc;
    int ret;

    enc = avcodec_find_encoder(priv->codec_id);
    if (!enc)
        return AVERROR(EINVAL);

    priv->enc_ctx = avcodec_alloc_context3(enc);
    if (!priv->enc_ctx)
        return AVERROR(ENOMEM);

    priv->enc_ctx->codec_type  = AVMEDIA_TYPE_AUDIO;
    priv->enc_ctx->sample_fmt  = priv->sample_fmt;
    priv->enc_ctx->sample_rate = priv->sample_rate;
    av_channel_layout_copy(&priv->enc_ctx->ch_layout, &priv->ch_layout);

    ret = avcodec_open2(priv->enc_ctx, enc, options);
    if (ret < 0) {
        avcodec_free_context(&priv->enc_ctx);
        return ret;
    }

    return 0;
}

static void tinycomprsink_close_encoder(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;

    if (!priv->enc_ctx)
        return;

    avcodec_free_context(&priv->enc_ctx);
}

static void tinycomprsink_control_callback(FAR void* cookie, int event, const FAR void* extra)
{
    AVFilterContext *ctx = (AVFilterContext *)cookie;
    CompSinkPriv *priv = ctx->priv;
    const char* audio_event_str[] = {
        [AUDIO_MSG_START]  = "STARTED",
        [AUDIO_MSG_PAUSE]  = "PAUSED",
        [AUDIO_MSG_RESUME] = "RESUMED",
        [AUDIO_MSG_COMPLETE] = "COMPLETE",
        [AUDIO_MSG_IOERR]  = "IOERR"
    };
    int i;

    av_log(ctx, AV_LOG_INFO, "tinycomprsink event:%s state:%s\n", audio_event_str[event],
           comp_sink_state_str[priv->state]);

    switch (event) {
        case AUDIO_MSG_START:
            if (priv->state == COMPSINK_STARTING)
                priv->state = COMPSINK_STARTED;
            break;
        case AUDIO_MSG_PAUSE:
            if (priv->state == COMPSINK_PAUSING)
                priv->state = COMPSINK_PAUSED;
            break;
        case AUDIO_MSG_RESUME:
            if (priv->state == COMPSINK_RESUMING)
                priv->state = COMPSINK_RESUMED;
            break;
        case AUDIO_MSG_IOERR:
            if (priv->state == COMPSINK_RESUMING) {
                /* Should stop output packet to device */
                for (i = 0; i < priv->nb_inputs; i++)
                    priv->input_state[i] = INPUT_PAUSED;
                priv->state = COMPSINK_PAUSED;
            }
            else if (priv->state == COMPSINK_STARTING) {
                for (i = 0; i < priv->nb_inputs; i++)
                    priv->input_state[i] = INPUT_PAUSED;
                priv->state = COMPSINK_STOPPED;
            }
    }
}

static int tinycomprsink_open(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    struct compr_config config = {0};
    struct snd_codec codec= {0};
    AVDictionary *fmt_opt = NULL;
    char devname[64];
    int ret;

    if (priv->compress)
        return 0;

    snprintf(devname, sizeof(devname), "/dev/audio/%s", priv->devname);
    priv->compress = compress_open_by_name(devname, COMPRESS_IN, NULL);
    if (!priv->compress || !is_compress_ready(priv->compress)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to open device node: %s\n", priv->devname);
        ret = AVERROR(EIO);
        goto out;
    }

    config.codec = &codec;
    ret = compress_get_current_config(priv->compress, &config);
    if (ret < 0)
        goto out;

    ret = compress_set_current_config(priv->compress, &config);
    if (ret < 0)
        goto out;

    av_dict_set_int(&fmt_opt, "ar", priv->sample_rate, 0);
    av_dict_set_int(&fmt_opt, "ac", priv->ch_layout.nb_channels, 0);
    av_dict_set_int(&fmt_opt, "ab", config.codec->bit_rate, 0);
    tinycomprsink_codec_to_options(ctx, config.codec, &fmt_opt);

    ret = tinycomprsink_open_encoder(ctx, &fmt_opt);
    av_dict_free(&fmt_opt);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "tinycomprsink fail to open encoder\n");
        goto out;
    }

    if (!priv->mix) {
        priv->mix = ff_amix_alloc(priv->sample_rate, priv->sample_fmt, priv->ch_layout.nb_channels);
        if (!priv->mix) {
            ret = AVERROR(ENOMEM);
            goto out;
        }
    }

    if (priv->enc_ctx->frame_size) {
        ff_amix_set_frame_size(priv->mix, priv->enc_ctx->frame_size);
        av_log(ctx, AV_LOG_INFO, "tinycomprsink frame_size:%d\n", priv->enc_ctx->frame_size);
    }

    compress_nonblock(priv->compress, 1);
    compress_set_event_callback(priv->compress, tinycomprsink_control_callback, ctx);
    priv->last_pkt = av_packet_alloc();
    if (!priv->last_pkt) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    priv->frame_count = 0;

    return 0;

out:
    if (priv->enc_ctx)
        avcodec_free_context(&priv->enc_ctx);
    av_packet_free(&priv->last_pkt);
    if (priv->compress) {
        compress_close(priv->compress);
        priv->compress = NULL;
    }

    if (priv->mix) {
        ff_amix_free(priv->mix);
        priv->mix = NULL;
    }

    return ret;
}

static int tinycomprsink_start(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    int ret;

    if (!priv->compress || priv->state != COMPSINK_STOPPED)
        return 0;

    ret = compress_start(priv->compress);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_INFO, "[%s:%d] %s start.\n", __func__, __LINE__, ctx->name);
    priv->state = COMPSINK_STARTING;
    priv->frame_count = 0;

    return 0;
}

static void tinycomprsink_close(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;

    av_log(ctx, AV_LOG_INFO, "%s close.\n", ctx->name);

    if (!priv->compress)
        return;

    if (priv->mix) {
        ff_amix_free(priv->mix);
        priv->mix = NULL;
    }

    av_packet_free(&priv->last_pkt);
    tinycomprsink_close_encoder(ctx);
    compress_drain(priv->compress);
    compress_close(priv->compress);
    priv->compress = NULL;
    priv->state = COMPSINK_STOPPED;
    priv->unlinked = false;
    priv->frame_count = 0;

    memset(priv->input_state, 0, sizeof(*priv->input_state) * priv->nb_inputs);
}

static int tinycomprsink_pause(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    int ret;

    if (!priv->compress || (priv->state != COMPSINK_STARTED && priv->state != COMPSINK_RESUMED))
        return 0;

    ret = compress_pause(priv->compress);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_INFO, "[%s:%d] %s pause.\n", __func__, __LINE__, ctx->name);
    priv->state = COMPSINK_PAUSING;
    priv->frame_count = 0;

    return 0;
}

static int tinycomprsink_resume(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    int ret;

    if (!priv->compress || priv->state != COMPSINK_PAUSED)
        return 0;

    ret = compress_resume(priv->compress);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_INFO, "[%s:%d] %s resume.\n", __func__, __LINE__, ctx->name);
    priv->state = COMPSINK_RESUMING;
    priv->frame_count = 0;

    return 0;
}

static int tinycomprsink_output_packet(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    AVPacket *pkt = priv->last_pkt;
    int ret = 0, i;

    if (!priv->compress)
        return 0;

    if (priv->state < COMPSINK_PAUSED) {
        av_log(ctx, AV_LOG_WARNING, "%s busy state:%s\n", ctx->name, comp_sink_state_str[priv->state]);
        return 0;
    }

    while (pkt) {
        if (pkt->data && !priv->unlinked) {
            ret = compress_write(priv->compress, pkt->data, pkt->size);
            if (ret == pkt->size)
                av_packet_unref(pkt);
            else if (ret > 0) {
                pkt->data += ret;
                pkt->size -= ret;
                break;
            } else if (ret == -EAGAIN) {
                if (priv->state == COMPSINK_STOPPED)
                    ret = tinycomprsink_start(ctx);
                else if (priv->state == COMPSINK_PAUSED)
                    ret = tinycomprsink_resume(ctx);

                if (ret < 0 && ret != -EAGAIN)
                    return ret;

                break;
            } else
                break;
        } else if (priv->unlinked) {
            if (pkt->data)
                av_packet_unref(pkt);
        }

        ret = avcodec_receive_packet(priv->enc_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN))
                ret = 0;
            break;
        }
    }

    if (ret == AVERROR_EOF) {
        for (i = 0; i < priv->nb_inputs; i++)
            ff_inlink_set_status(ctx->inputs[i], AVERROR_EOF);
        tinycomprsink_close(ctx);
    }

    return ret;
}

static int tinycomprsink_send_frame(AVFilterContext *ctx, AVFrame *frame)
{
    CompSinkPriv *priv = ctx->priv;
    int ret;

    ret = avcodec_send_frame(priv->enc_ctx, frame);
    if (ret < 0)
        return ret;

    return tinycomprsink_output_packet(ctx);
}

static int tinycomprsink_activate(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    int i, ret, count = 0;
    AVFrame *frame = NULL;
    AVFilterLink *link;
    int64_t pts;

    for (i = 0; i < priv->nb_inputs; i++) {
        if (priv->input_state[i] == INPUT_PAUSED) {
            if (ff_outlink_get_status(ctx->inputs[i]))
                ff_inlink_acknowledge_status(ctx->inputs[i], &ret, &pts);
            count ++;
        }
    }

    if (count == priv->nb_inputs &&
        (priv->state == COMPSINK_PAUSED || priv->state == COMPSINK_STOPPED)) {/* If all inputs arenot started, then skip output */
        av_log(ctx, AV_LOG_WARNING, "%s all inputs are not started\n", ctx->name);
        return 0;
    }

    ret = tinycomprsink_output_packet(ctx); /* Might cause silence frame before start recovered from pausing */
    if (ret < 0)
        return ret;

    for (i = 0; i < priv->nb_inputs; i++) {
        link = ctx->inputs[i];

        if (ff_inlink_check_available_frame(link)) {
            ret = tinycomprsink_open(ctx);
            if (ret < 0)
                return ret;

            ret = ff_amix_input_write(priv->mix, link);
            if (ret < 0) {
                av_log(ctx, AV_LOG_ERROR, "input[%d] write to mix failed, ret:%d.\n", i, ret);
                continue;
            }
        }
    }

    if (priv->state >= COMPSINK_PAUSED) {
        ff_amix_read(priv->mix, &frame);
        if (!frame && ff_amix_blocked(priv->mix)) {
            ret = tinycomprsink_pause(ctx);
            if (ret < 0)
                return ret;
        } else if (frame) {
            ret = tinycomprsink_send_frame(ctx, frame);
            av_frame_free(&frame);
            if (ret >= 0)
                ff_filter_set_ready(ctx, 100);
            return ret;
        }
    }

    for (i = 0; i < priv->nb_inputs; i++) {
        link = ctx->inputs[i];

        ff_inlink_acknowledge_status(link, &ret, &pts);
        if (ret >= 0) {
            if (ff_amix_input_want(priv->mix, link))
                ff_inlink_request_frame(link);
        }
    }

    return 0;
}

static int tinycomprsink_process_command(AVFilterContext *ctx,
                                         const char *cmd, const char *args,
                                         char *res, int res_len, int flags)
{
    CompSinkPriv *priv = ctx->priv;

    if (!strcmp(cmd, "get_pollfd")) {
        struct pollfd *poll_fd = (struct pollfd *)res;
        if(priv->compress) {
            poll_fd[0].fd = compress_get_file_descriptor(priv->compress);
            poll_fd[0].events = POLLIN;
            return 1;
        }
        return AVERROR(EINVAL);
    } else if (!strcmp(cmd, "poll_available")) {
        if (priv->compress) {
            if (priv->state == COMPSINK_PAUSING)
                av_log(ctx, AV_LOG_INFO, "[bt-audio]%s poll available when pausing.\n", ctx->name);
            compress_poll_available(priv->compress);
            ff_filter_set_ready(ctx, 100);
        }
        return 0;
    } else if (!strcmp(cmd, "unlink")) {
        if (priv->compress) {
            av_log(ctx, AV_LOG_INFO, "%s unlink.\n", ctx->name);
            priv->unlinked = true;
            tinycomprsink_send_frame(ctx, NULL);
        }
        return 0;
    }

    return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
}

static int tinycomprsink_forward_command(AVFilterContext *ctx,
                                         int pad_idx, const char* target, const char *cmd,
                                         const char *arg, char *res, int res_len, int flags)
{
    FilterLinkInternal *li = (FilterLinkInternal *)ctx->inputs[pad_idx];
    CompSinkPriv *priv = ctx->priv;
    const char* input_state_str[] = {
        [INPUT_PAUSED]  = "PAUSED",
        [INPUT_STARTED] = "STARTED"
    };


    if (!strcmp(cmd, "play")) {
        priv->input_state[pad_idx] = INPUT_STARTED;

        av_log(ctx, AV_LOG_INFO, "%s inputs[%d] recv play, input_state:%s sink_state:%s status_in:%d status_out:%d.\n",
               ctx->name, pad_idx, input_state_str[priv->input_state[pad_idx]],
               comp_sink_state_str[priv->state], li->status_in, li->status_out);
    } else if (!strcmp(cmd, "pause")) {
        if (priv->state < COMPSINK_PAUSED)
            priv->input_state[pad_idx] = INPUT_PAUSED;

        if (li->status_in)
            ff_inlink_set_status(ctx->inputs[pad_idx], AVERROR_EOF);

        av_log(ctx, AV_LOG_INFO, "%s inputs[%d] recv pause, input_state:%s sink_state:%s status_in:%d status_out:%d.\n",
            ctx->name, pad_idx, input_state_str[priv->input_state[pad_idx]],
            comp_sink_state_str[priv->state], li->status_in, li->status_out);
    }

    return 0;
}

static int tinycomprsink_query_cap(CompSinkPriv *priv, const char *format, int *out_value)
{
    AVOptionRanges* ranges = NULL;
    AVOptionRange* range = NULL;
    int ret;

    ret = alsa_query_caps(&ranges, priv->devname, format, false);
    if (ret > 0) {
        *out_value = ranges->range[0]->value_min;
        av_opt_freep_ranges(&ranges);
    }

    return ret;
}

static int tinycomprsink_query_formats(CompSinkPriv *priv)
{
    int codec_id;
    int ret;

    ret = tinycomprsink_query_cap(priv, "codec", &codec_id);
    if (ret < 0)
        return ret;

    priv->codec_id = codec_id;

    ret = tinycomprsink_query_cap(priv, "sample_fmts", &priv->sample_fmt);
    if (ret < 0)
        return ret;

    ret = tinycomprsink_query_cap(priv, "sample_rates", &priv->sample_rate);
    if (ret < 0)
        return ret;

    ret = tinycomprsink_query_cap(priv, "channels", &priv->ch_layout.nb_channels);
    if (ret < 0)
        return ret;

    av_channel_layout_default(&priv->ch_layout, priv->ch_layout.nb_channels);

    return 0;
}

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    const enum AVSampleFormat *sample_fmts = NULL;
    AVFilterFormats *formats = NULL;
    CompSinkPriv *priv = ctx->priv;
    AVCodecContext *enc_ctx = NULL;
    int fmt = 0, ret = 0, count;
    const AVCodec *enc;

    ret = tinycomprsink_query_formats(priv);
    if (ret < 0)
        return ret;

    enc = avcodec_find_encoder(priv->codec_id);
    if (!enc)
        return AVERROR_ENCODER_NOT_FOUND;

    enc_ctx = avcodec_alloc_context3(enc);
    if (!enc_ctx)
        return AVERROR(EINVAL);

    ret = avcodec_get_supported_config(enc_ctx, NULL,
                                        AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                        (const void **)&sample_fmts, &count);
    if (ret >= 0 && count > 0 && sample_fmts != NULL) {
        formats = ff_make_sample_format_list(sample_fmts);
        fmt = !!formats;
    }

    for (int i = 0; i < ctx->nb_inputs; i++) {
        const AVChannelLayout layout_list[] = { priv->ch_layout, { 0 } };
        ff_formats_unref(&cfg_in[i]->formats);
        if (fmt)
            ret = ff_formats_ref(formats, &cfg_in[i]->formats);
        else
            ret = ff_formats_ref(ff_make_format_list((const int[]){ priv->sample_fmt, -1 }), &cfg_in[i]->formats);

        if (ret < 0)
            goto out;

        ff_formats_unref(&cfg_in[i]->samplerates);
        ret = ff_formats_ref(ff_make_format_list((const int[]){ priv->sample_rate, -1 }), &cfg_in[i]->samplerates);
        if (ret < 0)
            goto out;

        ff_channel_layouts_unref(&cfg_in[i]->channel_layouts);
        ret = ff_channel_layouts_ref(ff_make_channel_layout_list(layout_list), &cfg_in[i]->channel_layouts);
    }

out:
    ff_formats_unref(&formats);
    avcodec_free_context(&enc_ctx);
    return ret;
}

static void tinycomprsink_uninit(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;

    av_freep(&priv->input_state);
}

static int tinycomprsink_init(AVFilterContext *ctx)
{
    CompSinkPriv *priv = ctx->priv;
    int i, ret;

    for (i = 0; i < priv->nb_inputs; i++) {
        AVFilterPad pad = { 0 };

        pad.type = AVMEDIA_TYPE_AUDIO;
        pad.name = av_asprintf("input%d", i);
        if (!pad.name)
            return AVERROR(ENOMEM);

        if ((ret = ff_append_inpad_free_name(ctx, &pad)) < 0)
            return ret;
    }

    priv->input_state = av_mallocz(priv->nb_inputs * sizeof(*priv->input_state));
    if (!priv->input_state)
        return AVERROR(ENOMEM);

    priv->state = COMPSINK_STOPPED;

    return 0;
}

#define OFFSET(x) offsetof(CompSinkPriv, x)
#define FLAGS  AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM
#define FLAGSR FLAGS|AV_OPT_FLAG_RUNTIME_PARAM
static const AVOption tinycomprsink_options[] = {
    {"inputs", "", OFFSET(nb_inputs), AV_OPT_TYPE_INT, {.i64 = 1}, 1, INT16_MAX, FLAGS},
    {"devname", "", OFFSET(devname), AV_OPT_TYPE_STRING, .flags = FLAGS},
    {"timeout", "timeout for force output", OFFSET(timeout), AV_OPT_TYPE_INT, {.i64 = 1000}, 0, INT32_MAX, FLAGS},
    {NULL},
};

AVFILTER_DEFINE_CLASS(tinycomprsink);

const AVFilter ff_asink_tinycomprsink = {
    .name            = "tinycomprsink",
    .description     = NULL_IF_CONFIG_SMALL("Audio tinycompress sink"),
    .priv_class      = &tinycomprsink_class,
    .priv_size       = sizeof(CompSinkPriv),
    .init            = tinycomprsink_init,
    .uninit          = tinycomprsink_uninit,
    FILTER_QUERY_FUNC2(query_formats),
    .activate        = tinycomprsink_activate,
    .process_command = tinycomprsink_process_command,
    .forward_command = tinycomprsink_forward_command,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL | AVFILTER_FLAG_DYNAMIC_INPUTS,
};
