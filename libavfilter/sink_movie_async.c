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
 * sink movie asynchronously
 */

#include <unistd.h>
#include <queue.h>

#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "movie_async.h"
#include "filters.h"

typedef struct MovieSinkCmd {
    struct dq_entry_s dq_entry;
    int               cmd;
    char              data[0];
} MovieSinkCmd;

typedef struct MovieStream {
    enum AVMediaType type;
    AVCodecContext   *enc_ctx;
    FFFrameQueue     dat_queue;
} MovieStream;

typedef struct MovieSinkPriv {
    const AVClass             *class;

    int                       cmd_max;
    int                       dat_max;
    int                       stack_size;
    int                       priority;

    AVFormatContext           *format_ctx;
    AVDictionary              *format_opt;
    AVOutputFormat            *format;
    MovieStream               *streams;
    AVDictionary              *global_opts;

    dq_queue_t                cmd_queue;         /**< graph thread send cmd to work thread */

    pthread_mutex_t           mutex;
    pthread_cond_t            cond;

    int                       state;
    void                      *cookie;
    unsigned                  current_ms;
    av_movie_async_event_func event;
} MovieSinkPriv;

static inline void amoviesink_notify_event(MovieSinkPriv *priv, int event, int ret, const char *extra)
{
    if (priv->event != NULL && priv->cookie != NULL)
        priv->event(priv->cookie, event, ret, extra);
}

static int amoviesink_send_cmd(AVFilterContext *ctx, int cmd, const void *data, size_t size)
{
    MovieSinkPriv *priv = ctx->priv;
    MovieSinkCmd *msg;

    msg = av_malloc(sizeof(MovieSinkCmd) + size);
    if (!msg)
        return AVERROR(ENOMEM);

    msg->cmd = cmd;

    if (data && size)
        memcpy(msg->data, data, size);

    pthread_mutex_lock(&priv->mutex);
    dq_addlast(&msg->dq_entry, &priv->cmd_queue);
    pthread_cond_signal(&priv->cond);
    pthread_mutex_unlock(&priv->mutex);

    return 0;
}

static int amoviesink_send_dat(AVFilterContext *ctx, int pad_id, AVFrame *frame)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret;

    pthread_mutex_lock(&priv->mutex);
    ret = ff_framequeue_add(&priv->streams[pad_id].dat_queue, frame);
    pthread_cond_signal(&priv->cond);
    pthread_mutex_unlock(&priv->mutex);

    return ret;
}

static AVFrame *amoviesink_recv_dat(AVFilterContext *ctx, int pad_id)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame = NULL;

    pthread_mutex_lock(&priv->mutex);
    if (ff_framequeue_queued_frames(&priv->streams[pad_id].dat_queue))
        frame = ff_framequeue_take(&priv->streams[pad_id].dat_queue);
    pthread_mutex_unlock(&priv->mutex);

    return frame;
}

static bool amoviesink_dat_full(AVFilterContext *ctx, int pad_id)
{
    MovieSinkPriv *priv = ctx->priv;
    bool full;

    pthread_mutex_lock(&priv->mutex);
    full = ff_framequeue_queued_frames(&priv->streams[pad_id].dat_queue) >= priv->dat_max;
    pthread_mutex_unlock(&priv->mutex);

    return full;
}

static bool amoviesink_dat_valid(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int i;

    if (priv->state != AVMOVIE_ASYNC_STATE_STARTED)
        return false;

    for (i = 0; i < ctx->nb_inputs; i++) {
        if (ff_framequeue_queued_frames(&priv->streams[i].dat_queue) > 0)
            return true;
    }

    return false;
}

static int amoviesink_clear_dat(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame;
    int i = 0;

    pthread_mutex_lock(&priv->mutex);
    for (i = 0; i < ctx->nb_inputs; i++) {
        while (ff_framequeue_queued_frames(&priv->streams[i].dat_queue)) {
            frame = ff_framequeue_take(&priv->streams[i].dat_queue);
            av_frame_free(&frame);
        }
    }
    pthread_mutex_unlock(&priv->mutex);

    return 0;
}

static int amoviesink_send_empty_frame(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame;
    int i, ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        frame = av_frame_alloc();
        if (!frame)
            return AVERROR(ENOMEM);

        ret = amoviesink_send_dat(ctx, i, frame);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }
    }

    return 0;
}

static void amoviesink_close_muxer(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int i;

    for (i = 0; i < ctx->nb_inputs; i++)
        avcodec_free_context(&priv->streams[i].enc_ctx);

    if (priv->format_ctx) {
        if (priv->format_ctx->pb)
            avio_close(priv->format_ctx->pb);

        avformat_free_context(priv->format_ctx);
        priv->format_ctx = NULL;
    }
}

static int amoviesink_open_muxer(AVFilterContext *ctx, const char *filename)
{
    MovieSinkPriv *priv = ctx->priv;
    AVDictionary *dict = NULL;
    int ret, i;

    ret = avformat_alloc_output_context2(&priv->format_ctx, priv->format,
                                         NULL, filename);
    if (ret < 0)
        return ret;

    priv->format_ctx->flags |= AVFMT_FLAG_NONBLOCK;
    priv->format_ctx->oformat->flags |= AVFMT_NOTIMESTAMPS;

    if (priv->global_opts)
        av_dict_copy(&dict, priv->global_opts, 0);

    ret = avio_open2(&priv->format_ctx->pb, filename, AVIO_FLAG_WRITE, NULL, &dict);
    av_dict_free(&dict);
    if (ret < 0)
        goto out;

    return 0;

out:
    avformat_free_context(priv->format_ctx);
    return ret;
}

static int amoviesink_open_encoder(AVFilterContext *ctx, int pad_id, const char *params)
{
    MovieSinkPriv *priv = ctx->priv;
    int format, sample_rate, channels, w, h;
    int vbr = -1, level = -1;
    uint64_t channel_layout;
    int64_t bitrate = -1;
    AVStream *stream;
    AVCodec *enc;
    int ret;

    if (priv->streams[pad_id].type == AVMEDIA_TYPE_AUDIO)
        enc = avcodec_find_encoder(priv->format_ctx->oformat->audio_codec);
    else
        enc = avcodec_find_encoder(priv->format_ctx->oformat->video_codec);

    if (!enc)
        return AVERROR(EINVAL);

    priv->streams[pad_id].enc_ctx = avcodec_alloc_context3(enc);
    if (!priv->streams[pad_id].enc_ctx)
        return AVERROR(ENOMEM);

    if (priv->streams[pad_id].type == AVMEDIA_TYPE_AUDIO) {
        sscanf(params, "a:%d,%d,%d,%llu,%lld,%d,%d",
               &format, &sample_rate, &channels, &channel_layout, &bitrate, &vbr, &level);

        priv->streams[pad_id].enc_ctx->sample_fmt     = format;
        priv->streams[pad_id].enc_ctx->sample_rate    = sample_rate;
        priv->streams[pad_id].enc_ctx->channels       = channels;
        priv->streams[pad_id].enc_ctx->channel_layout = channel_layout;
    } else {
        sscanf(params, "v:%d,%d,%d", &format, &w, &h);
        priv->streams[pad_id].enc_ctx->pix_fmt = format;
        priv->streams[pad_id].enc_ctx->width   = w;
        priv->streams[pad_id].enc_ctx->height  = h;
    }

    if (bitrate != -1)
        av_opt_set_int(priv->streams[pad_id].enc_ctx, "b", bitrate, 0);

    if (vbr != -1)
        av_opt_set_int(priv->streams[pad_id].enc_ctx, "vbr", vbr, AV_OPT_SEARCH_CHILDREN);

    if (level != -1)
        av_opt_set_int(priv->streams[pad_id].enc_ctx, "compression_level", level, 0);

    ret = avcodec_open2(priv->streams[pad_id].enc_ctx, enc, NULL);
    if (ret < 0)
        goto out;

    stream = avformat_new_stream(priv->format_ctx, enc);
    if (!stream) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    ret = avcodec_parameters_from_context(stream->codecpar, priv->streams[pad_id].enc_ctx);
    if (ret < 0)
        goto out;

    stream->time_base = (AVRational){ 1, priv->streams[pad_id].enc_ctx->sample_rate };
    return 0;

out:
    avcodec_free_context(&priv->streams[pad_id].enc_ctx);
    return ret;
}

static int amoviesink_open_encoders(AVFilterContext *ctx, const char *params)
{
    MovieSinkPriv *priv = ctx->priv;
    int i, ret = AVERROR(EINVAL);
    const char *param;

    for (i = 0; i < ctx->nb_inputs; i++)
    {
        if (priv->streams[i].type == AVMEDIA_TYPE_AUDIO)
            param = strchr(params, 'a');
        else
            param = strrchr(params, 'v');

        if (!param)
            goto out;

        ret = amoviesink_open_encoder(ctx, i, param);
        if (ret < 0)
            goto out;
    }

    ret = avformat_write_header(priv->format_ctx, NULL);
    if (ret < 0)
        goto out;

    return 0;

out:
    while (i-- > 0)
        avcodec_free_context(&priv->streams[i].enc_ctx);

    return ret;
}

static int amoviesink_encode_frame(AVFilterContext *ctx, int pad_id, AVFrame *frame)
{
    MovieSinkPriv *priv = ctx->priv;
    AVPacket pkt1, *pkt = &pkt1;
    int ret = 0;

    av_init_packet(pkt);

    ret = avcodec_send_frame(priv->streams[pad_id].enc_ctx, frame);
    if (ret < 0)
        goto out;

    while (1) {
        ret = avcodec_receive_packet(priv->streams[pad_id].enc_ctx, pkt);
        if (ret < 0)
            break;

        /* convert pts to time base of AVStream */
        av_packet_rescale_ts(pkt, priv->streams[pad_id].enc_ctx->time_base,
                                  priv->format_ctx->streams[pkt->stream_index]->time_base);

        if (pkt->pts >= 0)
            priv->current_ms = pkt->pts *
                               av_q2d(priv->format_ctx->streams[pkt->stream_index]->time_base) *
                               1000;

        ret = av_write_frame(priv->format_ctx, pkt);
        if (ret < 0)
            break;
    }

out:
    if (ret == AVERROR(EAGAIN))
        ret = 0;

    return ret;
}

static void amoviesink_prepare(AVFilterContext *ctx, const char *filename)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret = AVERROR(EPERM);

    if (priv->state != AVMOVIE_ASYNC_STATE_STOPPED)
        goto out;

    ret = amoviesink_open_muxer(ctx, filename);
    if (ret < 0)
        goto out;

    priv->state = AVMOVIE_ASYNC_STATE_PREPARED;

out:
    amoviesink_notify_event(priv, AVMOVIE_ASYNC_EVENT_PREPARED, ret, NULL);
}

static void amoviesink_start(AVFilterContext *ctx, const char *params)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret = AVERROR(EPERM);

    if (priv->state != AVMOVIE_ASYNC_STATE_PREPARED &&
        priv->state != AVMOVIE_ASYNC_STATE_PAUSED)
        goto out;

    if (priv->state == AVMOVIE_ASYNC_STATE_PREPARED) {
        ret = amoviesink_open_encoders(ctx, params);
        if (ret < 0)
            goto out;
    }

    priv->state = AVMOVIE_ASYNC_STATE_STARTED;
    ff_filter_set_ready(ctx, 100);
    ret = 0;

out:
    amoviesink_notify_event(priv, AVMOVIE_ASYNC_EVENT_STARTED, ret, NULL);
}

static void amoviesink_pause(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret = AVERROR(EPERM);

    if (priv->state != AVMOVIE_ASYNC_STATE_STARTED)
        goto out;

    priv->state = AVMOVIE_ASYNC_STATE_PAUSED;
    ret = 0;

out:
    amoviesink_notify_event(priv, AVMOVIE_ASYNC_EVENT_PAUSED, ret, NULL);
}

static int amoviesink_proc_dat(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame = NULL;
    int i, ret = 0;

    for (i = 0; i < ctx->nb_inputs; i++) {
        frame = amoviesink_recv_dat(ctx, i);
        if (!frame)
            continue;

        /* user request stop, send frame which linesize = 0 */
        if (!frame->linesize[0])
            av_frame_free(&frame);

        ret = amoviesink_encode_frame(ctx, i, frame);
        av_frame_free(&frame);
        if (ret < 0 && ret != AVERROR_EOF)
            goto out;
    }

    if (ret == AVERROR_EOF) {
        av_write_trailer(priv->format_ctx);
        goto out;
    }

    ff_filter_set_ready(ctx, 100);
    return 0;

out:
    amoviesink_clear_dat(ctx);

    priv->state      = AVMOVIE_ASYNC_STATE_COMPLETED;
    priv->current_ms = 0;
    amoviesink_notify_event(priv, AVMOVIE_ASYNC_EVENT_COMPLETED,
                            ret == AVERROR_EOF ? 0 : ret , NULL);

    return ret;
}

static void amoviesink_stop(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int i, ret = 0;

    if (priv->state == AVMOVIE_ASYNC_STATE_STOPPED)
        return;

    if (priv->state == AVMOVIE_ASYNC_STATE_PREPARED ||
        priv->state == AVMOVIE_ASYNC_STATE_COMPLETED)
        goto close_muxer;

    ret = amoviesink_send_empty_frame(ctx);
    while (ret == 0) {
        ret = amoviesink_proc_dat(ctx);
    }

close_muxer:
    amoviesink_close_muxer(ctx);
    priv->state = AVMOVIE_ASYNC_STATE_STOPPED;
    amoviesink_notify_event(priv, AVMOVIE_ASYNC_EVENT_STOPPED, 0, NULL);
}

static bool amoviesink_proc_cmd(AVFilterContext *ctx, MovieSinkCmd *msg)
{
    MovieSinkPriv *priv = ctx->priv;
    struct AVMovieAsyncEventCookie *event;
    bool exit = false;
    char *args;

    switch (msg->cmd) {
        case AVMOVIE_ASYNC_SET_EVENT:
            event = (AVMovieAsyncEventCookie *)msg->data;

            priv->event  = event->event;
            priv->cookie = event->cookie;
            break;

        case AVMOVIE_ASYNC_SET_OPTIONS:
            break;

        case AVMOVIE_ASYNC_PREPARE:
            amoviesink_prepare(ctx, msg->data);
            break;

        case AVMOVIE_ASYNC_START:
            amoviesink_start(ctx, msg->data);
            break;

        case AVMOVIE_ASYNC_PAUSE:
            amoviesink_pause(ctx);
            break;

        case AVMOVIE_ASYNC_CLOSE:
            exit = true;
        case AVMOVIE_ASYNC_STOP:
            amoviesink_stop(ctx);
            break;

        case AVMOVIE_ASYNC_PROCESS_COMMAND:
            args = strrchr(msg->data, '=');
            *args++ = '\0';
            ff_filter_process_command(ctx, msg->data, args, NULL, 0, 0);
            break;

        default:
            break;
    }

    av_freep(&msg);
    return exit;
}

static void *amoviesink_thread(void *arg)
{
    AVFilterContext *ctx = arg;
    MovieSinkPriv *priv = ctx->priv;
    MovieSinkCmd *msg;
    bool exit = false;

    while (1) {
        pthread_mutex_lock(&priv->mutex);

        if (dq_count(&priv->cmd_queue) > 0) {
            msg = (MovieSinkCmd *)dq_remfirst(&priv->cmd_queue);
            pthread_mutex_unlock(&priv->mutex);

            exit = amoviesink_proc_cmd(ctx, msg);
        } else if (amoviesink_dat_valid(ctx)) {
            pthread_mutex_unlock(&priv->mutex);

            amoviesink_proc_dat(ctx);
        } else if (exit) {
            priv->state  = AVMOVIE_ASYNC_STATE_NOP;
            priv->event  = NULL;
            priv->cookie = NULL;
            pthread_mutex_unlock(&priv->mutex);
            break;
        } else {
            pthread_cond_wait(&priv->cond, &priv->mutex);
            pthread_mutex_unlock(&priv->mutex);
        }
    }

    return NULL;
}

static void amoviesink_set_eof(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFilterLink *link;
    int i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        link = ctx->inputs[i];

        ff_inlink_set_status(link, AVERROR_EOF);
    }

    priv->format = NULL;

    if (priv->format_opt)
        av_dict_free(&priv->format_opt);
}

static int amoviesink_activate(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFilterLink *link;
    AVFrame *frame;
    int i, ret = 0;
    int64_t pts;

    for (i = 0; i < ctx->nb_inputs; i++) {
        if (amoviesink_dat_full(ctx, i))
            continue;

        link = ctx->inputs[i];
        ff_inlink_acknowledge_status(link, &ret, &pts);
        if (ret < 0)
            continue;

        ret = ff_inlink_consume_frame(link, &frame);
        if (ret > 0) {
            ret = amoviesink_send_dat(ctx, i, frame);
            if (ret < 0)
                av_frame_free(&frame);
        }

        if (ret < 0)
            continue;

        ff_inlink_request_frame(link);
    }

    return ret;
}

static void amoviesink_uninit(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int i;

    av_assert0(priv->state == AVMOVIE_ASYNC_STATE_NOP);

    for (i = 0; i < ctx->nb_inputs; i++) {
        ff_framequeue_free(&priv->streams[i].dat_queue);
        av_freep(&ctx->input_pads[i].name);
    }

    if (priv->global_opts)
        av_dict_free(&priv->global_opts);

    av_freep(&priv->streams);
    pthread_mutex_destroy(&priv->mutex);
    pthread_cond_destroy(&priv->cond);
}

static int amoviesink_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    MovieSinkPriv *priv = ctx->priv;
    enum AVMediaType types[] = {
        AVMEDIA_TYPE_AUDIO,
        AVMEDIA_TYPE_VIDEO,
    };
    AVFilterPad pad = { 0 };
    int i, inputs, ret;

    inputs = 1;
    if (ctx->filter->name[0] != 'a')
        inputs++;

    priv->streams = av_calloc(inputs, sizeof(MovieStream));
    if (!priv->streams)
        return AVERROR(ENOMEM);

    dq_init(&priv->cmd_queue);
    pthread_mutex_init(&priv->mutex, NULL);
    pthread_cond_init(&priv->cond, NULL);

    for (i = 0; i < inputs; i++) {
        priv->streams[i].type = types[i];
        ff_framequeue_init(&priv->streams[i].dat_queue, NULL);

        pad.type = types[i];
        pad.name = av_asprintf("input%d", i);
        if (!pad.name) {
            ret = AVERROR(ENOMEM);
            goto out;
        }

        if ((ret = ff_insert_inpad(ctx, i, &pad)) < 0) {
            av_freep(&pad.name);
            goto out;
        }
    }

    if (options && *options) {
        av_dict_copy(&priv->global_opts, *options, 0);
        av_dict_free(options);
    }

    return 0;

out:
    amoviesink_uninit(ctx);
    return ret;
}

static int amoviesink_query_audio_fmts(AVFilterContext *ctx, int pad_id, enum AVCodecID codec_id)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFilterLink *link = ctx->inputs[pad_id];
    AVFilterChannelLayouts *layouts;
    AVFilterFormats *formats;
    AVDictionaryEntry *tag;
    AVCodec *enc;

    int64_t value64 = 0, list64[] = { 0, -1 }, *list_i64;
    int value = 0, list[] = { 0, -1 }, *list_i32;
    bool supported;
    int n, ret;

    enc = avcodec_find_encoder(codec_id);
    if (!enc)
        return AVERROR(EINVAL);

    /* sample format */
    supported = false;
    if ((tag = av_dict_get(priv->format_opt, "sample_fmt", NULL, 0))) {
        if ((ret = ff_parse_sample_format(&value, tag->value, ctx)) < 0)
            return ret;
    }

    if (value) {
        if (enc->sample_fmts)
            supported = ff_fmt_is_in(value, enc->sample_fmts);
        else
            supported = true;
    }

    if (supported) {
        list[0] = value;
        formats = ff_make_format_list(list);
    } else {
        formats = enc->sample_fmts ?
                  ff_make_format_list(enc->sample_fmts) : ff_all_formats(AVMEDIA_TYPE_AUDIO);
    }

    if (ret = ff_formats_ref(formats, &link->out_formats) < 0)
        return ret;

    /* sample rate */
    supported = false;
    value = 0;
    if ((tag = av_dict_get(priv->format_opt, "sample_rate", NULL, 0))) {
        if ((ret = ff_parse_sample_rate(&value, tag->value, ctx)) < 0)
            return ret;
    }

    if (value) {
        if (enc->supported_samplerates)
            supported = ff_fmt_is_in(value, enc->supported_samplerates);
        else
            supported = true;
    }

    if (supported) {
        list[0] = value;
        formats = ff_make_format_list(list);
    } else {
        if (enc->supported_samplerates) {
            n = 0;
            while (enc->supported_samplerates[n] != 0)
                n++;

            list_i32 = av_mallocz_array(n + 1, sizeof(enc->supported_samplerates[0]));
            if (!list_i32)
                return AVERROR(ENOMEM);

            memcpy(list_i32, enc->supported_samplerates, n * sizeof(enc->supported_samplerates[0]));
            list_i32[n] = -1;

            formats = ff_make_format_list(list_i32);
            av_freep(&list_i32);
        } else {
            formats = ff_all_samplerates();
        }
    }

    if (ret = ff_formats_ref(formats, &link->out_samplerates) < 0)
        return ret;

    /* channel layout */
    supported = false;
    if ((tag = av_dict_get(priv->format_opt, "channel_layout", NULL, 0))) {
        if ((ret = ff_parse_channel_layout(&value64, NULL,
                                            tag->value, ctx)) < 0)
            return ret;
    }

    if (value64) {
        if (enc->channel_layouts) {
            n = 0;
            while (enc->channel_layouts[n] != 0) {
                if (value64 == enc->channel_layouts[n++]) {
                    supported = true;
                    break;
                }
            }
        } else {
            supported = true;
        }
    }

    if (supported) {
        list64[0] = value64;
        layouts = avfilter_make_format64_list(list64);
    } else {
        if (enc->channel_layouts) {
            n = 0;
            while (enc->channel_layouts[n])
                n++;

            list_i64 = av_mallocz_array(n + 1, sizeof(enc->channel_layouts[0]));
            if (!list_i64)
                return AVERROR(ENOMEM);

            memcpy(list_i64, enc->channel_layouts, n * sizeof(enc->channel_layouts[0]));
            list_i64[n] = -1;
            layouts = avfilter_make_format64_list(list_i64);
            av_freep(&list_i64);
        } else {
            layouts = ff_all_channel_counts();
        }
    }

    return ff_channel_layouts_ref(layouts, &link->out_channel_layouts);
}

static int amoviesink_query_video_fmts(AVFilterContext *ctx, int pad_id, enum AVCodecID codec_id)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFilterFormats *formats;
    AVFilterLink *link;
    AVCodec *enc;
    int ret;

    link = ctx->inputs[pad_id];
    enc  = avcodec_find_encoder(codec_id);
    if (!enc)
        return AVERROR(EINVAL);

    if (enc->pix_fmts) {
        formats = ff_make_format_list(enc->pix_fmts);
    } else {
        formats = ff_all_formats(AVMEDIA_TYPE_VIDEO);
    }

    return ff_formats_ref(formats, &link->out_formats);
}

static int amoviesink_query_formats(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFilterLink *link;
    int ret, i;

    if (!priv->format)
        return FFERROR_NOT_READY;

    for (i = 0; i < ctx->nb_inputs; i++) {
        link  = ctx->inputs[i];
        switch (link->type) {
            case AVMEDIA_TYPE_AUDIO:
                ret = amoviesink_query_audio_fmts(ctx, i, priv->format->audio_codec);
                break;
            case AVMEDIA_TYPE_VIDEO:
                ret = amoviesink_query_video_fmts(ctx, i, priv->format->video_codec);
                break;
            default:
                break;
        }
    }

    return ret;
}

static int amoviesink_process_open(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    struct sched_param param;
    pthread_attr_t attr;
    pthread_t thread;
    int ret;

    ret = amoviesink_send_cmd(ctx, AVMOVIE_ASYNC_OPEN, NULL, 0);

    if (priv->state != AVMOVIE_ASYNC_STATE_NOP)
        return ret;

    priv->state = AVMOVIE_ASYNC_STATE_STOPPED;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, priv->stack_size);
    param.sched_priority = priv->priority;
    pthread_attr_setschedparam(&attr, &param);
    ret = pthread_create(&thread, &attr, amoviesink_thread, ctx);
    if (ret != 0) {
        priv->state = AVMOVIE_ASYNC_STATE_NOP;
        return AVERROR(ret);
    }

    pthread_setname_np(thread, "media_sink_movie");
    pthread_detach(thread);
    return 0;
}

static int amoviesink_process_prepare(AVFilterContext *ctx, const char *args)
{
    MovieSinkPriv *priv = ctx->priv;
    AVDictionaryEntry *tag;
    char *format = NULL;

    if ((tag = av_dict_get(priv->format_opt, "format", NULL, 0)))
        format = tag->value;

    priv->format = av_guess_format(format, args, NULL);
    if (!priv->format)
        return AVERROR(EINVAL);

    return amoviesink_send_cmd(ctx, AVMOVIE_ASYNC_PREPARE, args, strlen(args) + 1);
}

static int amoviesink_process_start(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int i, ret, len, vbr = -1, level = -1;
    int64_t bitrate = -1, pts;
    AVDictionaryEntry *tag;
    bool reconfig = false;
    char *ptr, params[64];
    AVFilterLink *link;

    for (i = 0; i < ctx->nb_inputs; i++) {
        link = ctx->inputs[i];

        ff_inlink_acknowledge_status(link, &ret, &pts);
        if (ret < 0) {
            reconfig = true;
            break;
        }
    }

    if (reconfig)
        avfilter_graph_reconfig(ctx->graph, NULL);

    if ((tag = av_dict_get(priv->format_opt, "bitrate", NULL, 0)))
        bitrate = strtoul(tag->value, NULL, 0);

    if ((tag = av_dict_get(priv->format_opt, "vbr", NULL, 0)))
        vbr = strtoul(tag->value, NULL, 0);

    if ((tag = av_dict_get(priv->format_opt, "level", NULL, 0)))
        level = strtoul(tag->value, NULL, 0);

    ptr = params;
    len = sizeof(params);
    for (i = 0; i < ctx->nb_inputs; i++) {
        link = ctx->inputs[i];
        if (link->type == AVMEDIA_TYPE_AUDIO)
            ret = snprintf(ptr, len, "a:%d,%d,%d,%llu,%lld,%d,%d",
                           link->format, link->sample_rate, link->channels, link->channel_layout, bitrate, vbr, level);

        else
            ret = snprintf(ptr, len, ";v:%d,%d,%d", link->format, link->w, link->h);

        ptr += ret;
        len -= ret;
    }

    return amoviesink_send_cmd(ctx, AVMOVIE_ASYNC_START, params, strlen(params) + 1);
}

static int amoviesink_process_pause(AVFilterContext *ctx)
{
    return amoviesink_send_cmd(ctx, AVMOVIE_ASYNC_PAUSE, NULL, 0);
}

static int amoviesink_process_quit(AVFilterContext *ctx, const char *cmd)
{
    MovieSinkPriv *priv = ctx->priv;
    bool reset, close;
    MovieSinkCmd *msg;
    int ret;

    reset = !strcmp(cmd, "reset");
    close = !strcmp(cmd, "close");

    if (reset || close) {
        pthread_mutex_lock(&priv->mutex);
        while ((msg = (MovieSinkCmd *)dq_remfirst(&priv->cmd_queue)) != NULL)
            av_freep(&msg);
        pthread_mutex_unlock(&priv->mutex);
    }

    if (close)
        ret = amoviesink_send_cmd(ctx, AVMOVIE_ASYNC_CLOSE, NULL, 0);
    else
        ret = amoviesink_send_cmd(ctx, AVMOVIE_ASYNC_STOP, NULL, 0);

    if (ret < 0)
        return ret;

    amoviesink_set_eof(ctx);
    return ret;
}

static int amoviesink_process_process_command(AVFilterContext *ctx, const char *cmd, const char *args)
{
    MovieSinkPriv *priv = ctx->priv;
    int len, ret;
    char *ptr;

    if (!cmd || !args)
        return AVERROR(EINVAL);

    len = strlen(cmd) + (args ? strlen(args) : 1) + 2;
    ptr = av_malloc(len);
    if (!ptr)
        return AVERROR(ENOMEM);

    snprintf(ptr, len, "%s=%s", cmd, args);
    ret = amoviesink_send_cmd(ctx, AVMOVIE_ASYNC_PROCESS_COMMAND, ptr, len);
    free(ptr);

    return ret;
}

static int amoviesink_get_position(AVFilterContext *ctx, char *res, int res_len)
{
    MovieSinkPriv *priv = ctx->priv;

    if (!res || !res_len)
        return AVERROR(EINVAL);

    snprintf(res, res_len, "%u", priv->current_ms);
    return 0;
}

static int amoviesink_process_dump(AVFilterContext *ctx, char *res, int res_len)
{
    MovieSinkPriv *priv = ctx->priv;
    int pos = 0, ret, i;

    ret = snprintf(res, res_len, "st: %d", priv->state);
    pos += ret;

    if (priv->state != AVMOVIE_ASYNC_EVENT_STARTED)
        return 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (priv->streams[i].enc_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            ret = snprintf(res + pos, res_len - pos, ", A: %s %d %d %d",
                                    avcodec_get_name(priv->streams[i].enc_ctx->codec_id),
                                    priv->streams[i].enc_ctx->sample_rate,
                                    priv->streams[i].enc_ctx->channels,
                                    ff_framequeue_queued_frames(&priv->streams[i].dat_queue));
        } else {
            ret = snprintf(res + pos, res_len - pos, ", V: %s %d %d %d",
                                    avcodec_get_name(priv->streams[i].enc_ctx->codec_id),
                                    priv->streams[i].enc_ctx->width,
                                    priv->streams[i].enc_ctx->height,
                                    ff_framequeue_queued_frames(&priv->streams[i].dat_queue));
        }

        if (ret < 0)
            return ret;

        pos += ret;
        if (pos >= res_len)
            break;
    }

    return 0;
}

static int amoviesink_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                      char *res, int res_len, int flags)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret;

    if (!strcmp(cmd, "open")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s open.\n", __func__, ctx->name);
        return amoviesink_process_open(ctx);
    } else if (!strcmp(cmd, "set_event")) {
        if (!args)
            return AVERROR(EINVAL);

        return amoviesink_send_cmd(ctx, AVMOVIE_ASYNC_SET_EVENT, args, sizeof(struct AVMovieAsyncEventCookie));
    } else if (!strcmp(cmd, "set_options")) {
        return av_dict_parse_string(&priv->format_opt, args, "=", ":", 0);
    } else if (!strcmp(cmd, "prepare")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s prepare %s.\n", __func__, ctx->name, args);
        return amoviesink_process_prepare(ctx, args);
    } else if (!strcmp(cmd, "start")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s start.\n", __func__, ctx->name);
        return amoviesink_process_start(ctx);
    } else if (!strcmp(cmd, "pause")) {
        return amoviesink_process_pause(ctx);
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "reset") || !strcmp(cmd, "close")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s %s. pos %d\n", __func__, ctx->name, cmd, priv->current_ms);
        return amoviesink_process_quit(ctx, cmd);
    } else if (!strcmp(cmd, "get_position")) {
        return amoviesink_get_position(ctx, res, res_len);
    } else if (!strcmp(cmd, "dump")) {
        return amoviesink_process_dump(ctx, res, res_len);
    } else if (!res && !res_len) {
        return amoviesink_process_process_command(ctx, cmd, args);
    } else {
        return AVERROR(ENOSYS);
    }
}

#define OFFSET(x) offsetof(MovieSinkPriv, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption amoviesink_async_options[] = {
    { "datqmax",    "maximum number of dat queue", OFFSET(dat_max),    AV_OPT_TYPE_INT,    {.i64 = 4 },      2, 8,         FLAGS },
    { "cmdqmax",    "maximum number of cmd queue", OFFSET(cmd_max),    AV_OPT_TYPE_INT,    {.i64 = 16 },     8, 32,        FLAGS },
    { "stack_size", "stack size of work thread",   OFFSET(stack_size), AV_OPT_TYPE_INT,    {.i64 = 61440 },  0, INT32_MAX, FLAGS },
    { "priority",   "priority of work thread",     OFFSET(priority),   AV_OPT_TYPE_INT,    {.i64 = 244 },    0, INT16_MAX, FLAGS },
    { NULL },
};

static const struct AVClass *amoviesink_child_class_next(const struct AVClass *prev)
{
    if (!prev)
        return avformat_get_class();
    else if (prev == avformat_get_class())
        return avcodec_get_class();
    else
        return NULL;
}

static void *amoviesink_child_next(void *obj, void *prev)
{
    MovieSinkPriv *priv = obj;

    if (!prev) {
        return priv->format_ctx;
    } else if (prev == priv->format_ctx) {
        return priv->streams[0].enc_ctx;
    } else {
        return NULL;
    }
}

#if CONFIG_AMOVIESINK_ASYNC_FILTER

static const AVClass amoviesink_async_class = {
    .class_name       = "amoviesink_async_class",
    .item_name        = av_default_item_name,
    .option           = amoviesink_async_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
    .child_next       = amoviesink_child_next,
    .child_class_next = amoviesink_child_class_next
};

AVFilter ff_sink_amoviesink_async = {
    .name            = "amoviesink_async",
    .description     = NULL_IF_CONFIG_SMALL("amovie sink asyncchronously, end of the filter graph."),
    .priv_class      = &amoviesink_async_class,
    .priv_size       = sizeof(MovieSinkPriv),
    .init_dict       = amoviesink_init_dict,
    .uninit          = amoviesink_uninit,
    .query_formats   = amoviesink_query_formats,
    .activate        = amoviesink_activate,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .process_command = amoviesink_process_command,
};
#endif

#if CONFIG_MOVIESINK_ASYNC_FILTER

static const AVClass moviesink_async_class = {
    .class_name       = "moviesink_async_class",
    .item_name        = av_default_item_name,
    .option           = amoviesink_async_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
    .child_next       = amoviesink_child_next,
    .child_class_next = amoviesink_child_class_next
};

AVFilter ff_sink_moviesink_async = {
    .name            = "moviesink_async",
    .description     = NULL_IF_CONFIG_SMALL("movie sink asyncchronously, end of the filter graph."),
    .priv_class      = &moviesink_async_class,
    .priv_size       = sizeof(MovieSinkPriv),
    .init_dict       = amoviesink_init_dict,
    .uninit          = amoviesink_uninit,
    .query_formats   = amoviesink_query_formats,
    .activate        = amoviesink_activate,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .process_command = amoviesink_process_command,
};
#endif
