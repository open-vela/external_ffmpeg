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

#include "config_components.h"

#include <unistd.h>
#include <pthread.h>
#include <sys/queue.h>
#include <pthread.h>

#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include "filters.h"
#include "movie_async.h"
#include "packet_wrapper.h"

typedef struct MovieSinkCmd {
    SIMPLEQ_ENTRY(MovieSinkCmd) entry;
    int                         cmd;
    char                        data[0];
} MovieSinkCmd;

typedef struct MovieSinkEvent {
    SIMPLEQ_ENTRY(MovieSinkEvent) entry;
    int                       event;
    int                       ret;
    char                      *extra;
    char                      data[0];
} MovieSinkEvent;

SIMPLEQ_HEAD(MovieSinkCmdQueue, MovieSinkCmd);
SIMPLEQ_HEAD(MovieSinkEvtQueue, MovieSinkEvent);

typedef struct MovieStream {
    enum AVMediaType type;
    FFFrameQueue     dat_queue;
    int64_t          sync_pts;
    int              stream_idx;
    int64_t          last_pts;
} MovieStream;

typedef struct MovieSinkPriv {
    const AVClass             *class;

    int                       cmd_max;
    int                       dat_max;
    int                       stack_size;
    int                       priority;

    const AVOutputFormat      *format;
    AVFormatContext           *format_ctx;
    AVDictionary              *format_opt;
    MovieStream               *streams;
    AVDictionary              *global_opts;

    struct MovieSinkCmdQueue  cmd_queue;    /**< graph thread send cmd to work thread */
    struct MovieSinkEvtQueue  evt_queue;    /**< event queue which worker thread to mediad */

    pthread_mutex_t           mutex;
    pthread_cond_t            cond;

    int                       state;
    void                      *cookie;
    unsigned                  current_ms;
    av_movie_async_event_func event;
} MovieSinkPriv;

static inline void moviesink_notify_event(MovieSinkPriv *priv, int event, int ret, const char *extra)
{
    if (priv->event != NULL && priv->cookie != NULL) {
        priv->event(priv->cookie, event, ret, extra);

        if (event == AVMOVIE_ASYNC_EVENT_CLOSED) {
            priv->event  = NULL;
            priv->cookie = NULL;
        }
    }
}

static int moviesink_send_cmd(AVFilterContext *ctx, int cmd, const void *data, size_t size)
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
    SIMPLEQ_INSERT_TAIL(&priv->cmd_queue, msg, entry);
    pthread_cond_signal(&priv->cond);
    pthread_mutex_unlock(&priv->mutex);

    return 0;
}

static int moviesink_send_dat(AVFilterContext *ctx, int pad_id, AVFrame *frame)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret;

    pthread_mutex_lock(&priv->mutex);
    ret = ff_framequeue_add(&priv->streams[pad_id].dat_queue, frame);
    pthread_cond_signal(&priv->cond);
    pthread_mutex_unlock(&priv->mutex);

    return ret;
}

static int moviesink_send_event(AVFilterContext *ctx, int event, int ret, const char *extra)
{
    MovieSinkPriv *priv = ctx->priv;
    MovieSinkEvent *evt;

    if (extra)
        evt = av_malloc(sizeof(MovieSinkEvent) + strlen(extra) + 1);
    else
        evt = av_malloc(sizeof(MovieSinkEvent));
    if (!evt)
        return AVERROR(ENOMEM);

    evt->event = event;
    evt->ret   = ret;
    evt->extra = NULL;

    if (extra) {
        evt->extra = evt->data;
        strcpy(evt->extra, extra);
    }

    pthread_mutex_lock(&priv->mutex);
    SIMPLEQ_INSERT_TAIL(&priv->evt_queue, evt, entry);
    pthread_mutex_unlock(&priv->mutex);

    ff_filter_set_ready(ctx, 100);
    return 0;
}

static void moviesink_set_eof(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFilterLink *link;
    int i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        link = ctx->inputs[i];

        ff_inlink_set_status(link, AVERROR_EOF);
    }
}

static void moviesink_proc_event(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;

    while (1) {
        MovieSinkEvent *event;

        pthread_mutex_lock(&priv->mutex);
        if ((event = SIMPLEQ_FIRST(&priv->evt_queue)) != NULL)
            SIMPLEQ_REMOVE_HEAD(&priv->evt_queue, entry);
        pthread_mutex_unlock(&priv->mutex);

        if (!event)
            break;

        if (event->event == AVMOVIE_ASYNC_EVENT_COMPLETED)
            moviesink_set_eof(ctx);

        moviesink_notify_event(priv, event->event, event->ret, event->extra);
        av_freep(&event);
    }
}

static AVFrame *moviesink_recv_dat(AVFilterContext *ctx, int pad_id)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame = NULL;

    pthread_mutex_lock(&priv->mutex);
    if (ff_framequeue_queued_frames(&priv->streams[pad_id].dat_queue))
        frame = ff_framequeue_take(&priv->streams[pad_id].dat_queue);
    pthread_mutex_unlock(&priv->mutex);

    return frame;
}

static AVFrame *moviesink_peek_dat(AVFilterContext *ctx, int pad_id)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame = NULL;

    pthread_mutex_lock(&priv->mutex);
    if (ff_framequeue_queued_frames(&priv->streams[pad_id].dat_queue))
        frame = ff_framequeue_peek(&priv->streams[pad_id].dat_queue, 0);
    pthread_mutex_unlock(&priv->mutex);

    return frame;
}

static bool moviesink_dat_full(AVFilterContext *ctx, int pad_id)
{
    MovieSinkPriv *priv = ctx->priv;
    bool full;

    pthread_mutex_lock(&priv->mutex);
    full = ff_framequeue_queued_frames(&priv->streams[pad_id].dat_queue) >= priv->dat_max;
    pthread_mutex_unlock(&priv->mutex);

    return full;
}

static bool moviesink_dat_valid(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int i;

    if (priv->state != AVMOVIE_ASYNC_STATE_STARTED &&
        priv->state != AVMOVIE_ASYNC_STATE_PAUSED)
        return false;

    for (i = 0; i < ctx->nb_inputs; i++) {
        if (ff_framequeue_queued_frames(&priv->streams[i].dat_queue) > 0)
            return true;
    }

    return false;
}

static int moviesink_clear_dat(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame;
    int i;

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

static int moviesink_send_empty_frame(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame;
    int i, ret = AVERROR(EPERM);

    if (priv->state != AVMOVIE_ASYNC_STATE_STARTED &&
        priv->state != AVMOVIE_ASYNC_STATE_PAUSED)
        return ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        frame = av_frame_alloc();
        if (!frame)
            return AVERROR(ENOMEM);

        ret = moviesink_send_dat(ctx, i, frame);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }
    }

    return ret;
}

static void moviesink_close_muxer(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int i;

    for (i = 0; i < ctx->nb_inputs; i++) {
        priv->streams[i].sync_pts = 0;
        priv->streams[i].stream_idx = -1;
    }

    if (priv->format_ctx) {
        if (priv->format_ctx->pb)
            avio_close(priv->format_ctx->pb);

        avformat_free_context(priv->format_ctx);
        priv->format_ctx = NULL;
    }
}

static int moviesink_async_interrupt(void *opaque)
{
    AVFilterContext *ctx = opaque;
    MovieSinkPriv *priv = ctx->priv;
    MovieSinkCmd *msg;
    int interrupt = 0;

    if (priv->state == AVMOVIE_ASYNC_STATE_STARTED)
        goto out;

    pthread_mutex_lock(&priv->mutex);
    SIMPLEQ_FOREACH(msg, &priv->cmd_queue, entry) {
        if (msg->cmd >= AVMOVIE_ASYNC_STOP) {
            interrupt = 1;
            break;
        }
    }

    pthread_mutex_unlock(&priv->mutex);

out:
    return interrupt;
}

static int moviesink_open_muxer(AVFilterContext *ctx, const char *filename)
{
    MovieSinkPriv *priv = ctx->priv;
    AVDictionary *dict = NULL;
    AVIOInterruptCB cb;
    int ret, i;

    for (i = 0; i < ctx->nb_inputs; i++)
        priv->streams[i].last_pts = AV_NOPTS_VALUE;

    ret = avformat_alloc_output_context2(&priv->format_ctx, priv->format,
                                         NULL, filename);
    if (ret < 0)
        return ret;

    priv->format_ctx->flags |= AVFMT_FLAG_NONBLOCK;

    cb.callback = moviesink_async_interrupt;
    cb.opaque   = ctx;

    pthread_mutex_lock(&priv->mutex);
    if (priv->global_opts)
        av_dict_copy(&priv->format_opt, priv->global_opts, 0);

    if (priv->format_opt) {
        av_dict_copy(&dict, priv->format_opt, 0);
        ret = av_opt_set_dict2(priv->format_ctx, &dict, AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            pthread_mutex_unlock(&priv->mutex);
            av_dict_free(&dict);
            goto out;
        }
    }
    pthread_mutex_unlock(&priv->mutex);

    ret = avio_open2(&priv->format_ctx->pb, filename, AVIO_FLAG_WRITE, &cb, &dict);
    av_dict_free(&dict);
    if (ret < 0)
        goto out;

    return 0;

out:
    avformat_free_context(priv->format_ctx);
    return ret;
}

static int moviesink_init_stream(AVFilterContext *ctx, int pad_id, AVFrame *frame)
{
    MovieSinkPriv *priv = ctx->priv;
    AVCodecParameters *params;
    AVStream *stream;
    int ret;

    if (priv->streams[pad_id].stream_idx >= 0)
        return 0;

    unwrap_frame(frame, NULL, &params);
    if (!params)
        return AVERROR(EINVAL);

    stream = avformat_new_stream(priv->format_ctx, NULL);
    if (!stream)
        return AVERROR(ENOMEM);

    stream->time_base = ctx->inputs[pad_id]->time_base;
    priv->streams[pad_id].stream_idx = priv->format_ctx->nb_streams - 1;

    ret = avcodec_parameters_copy(stream->codecpar, params);
    if (ret < 0)
        return ret;

    return 1;
}

static int moviesink_write_frame(AVFilterContext *ctx, int pad_id, AVFrame *frame)
{
    MovieSinkPriv *priv = ctx->priv;
    AVPacket *pkt;

    unwrap_frame(frame, &pkt, NULL);
    pkt->pts -= priv->streams[pad_id].sync_pts;
    pkt->dts -= priv->streams[pad_id].sync_pts;

    pkt->stream_index = priv->streams[pad_id].stream_idx;

    /* convert timebase from stream to container. */
    av_packet_rescale_ts(pkt, ctx->inputs[pad_id]->time_base,
                              priv->format_ctx->streams[pkt->stream_index]->time_base);

    if (pkt->pts >= 0)
        priv->current_ms = av_rescale_q(pkt->pts,
                                        priv->format_ctx->streams[pkt->stream_index]->time_base,
                                        av_make_q(1, 1000));

    return av_write_frame(priv->format_ctx, pkt);
}

static void moviesink_prepare(AVFilterContext *ctx, const char *filename)
{
    MovieSinkPriv *priv = ctx->priv;
    AVDictionaryEntry *tag;
    char *format = NULL;
    int ret = AVERROR(EPERM);

    if (priv->state != AVMOVIE_ASYNC_STATE_STOPPED)
        goto out;

    if ((tag = av_dict_get(priv->format_opt, "format", NULL, 0)))
        format = tag->value;

    priv->format = av_guess_format(format, filename, NULL);
    if (!priv->format) {
        ret = AVERROR(EINVAL);
        goto out;
    }

    ret = moviesink_open_muxer(ctx, filename);
    if (ret < 0)
        goto out;

    priv->state = AVMOVIE_ASYNC_STATE_PREPARED;

out:
    moviesink_send_event(ctx, AVMOVIE_ASYNC_EVENT_PREPARED, ret, NULL);
}

static void moviesink_start(AVFilterContext *ctx, const char *params)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret = AVERROR(EPERM);

    if (priv->state != AVMOVIE_ASYNC_STATE_PREPARED &&
        priv->state != AVMOVIE_ASYNC_STATE_PAUSED)
        goto out;

    priv->state = AVMOVIE_ASYNC_STATE_STARTED;
    ret = 0;

out:
    moviesink_send_event(ctx, AVMOVIE_ASYNC_EVENT_STARTED, ret, NULL);
}

static void moviesink_pause(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret = AVERROR(EPERM);

    if (priv->state != AVMOVIE_ASYNC_STATE_STARTED)
        goto out;

    priv->state = AVMOVIE_ASYNC_STATE_PAUSED;
    ret = 0;

out:
    moviesink_send_event(ctx, AVMOVIE_ASYNC_EVENT_PAUSED, ret, NULL);
}

static void moviesink_clean(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    if (priv->state != AVMOVIE_ASYNC_STATE_STOPPED) {
        moviesink_close_muxer(ctx);
        priv->state = AVMOVIE_ASYNC_STATE_STOPPED;
        moviesink_send_event(ctx, AVMOVIE_ASYNC_EVENT_STOPPED, 0, NULL);
    }
}

static int moviesink_proc_dat(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVFrame *frame = NULL;
    int i, ret = 0;

    for (i = 0; i < ctx->nb_inputs; i++) {
        frame = moviesink_peek_dat(ctx, i);
        if (!frame)
            continue;

        /* @deprecated naive avsync. */
        ret = moviesink_init_stream(ctx, i, frame);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "moviesink_init_stream error %d\n.", ret);
            frame = moviesink_recv_dat(ctx, i);
            av_frame_free(&frame);
            priv->state  = AVMOVIE_ASYNC_STATE_COMPLETED;
            priv->format = NULL;
            moviesink_send_event(ctx, AVMOVIE_ASYNC_EVENT_COMPLETED, ret, NULL);
            return ret;
        } else if (priv->format_ctx->nb_streams < ctx->nb_inputs) {
            priv->streams[i].sync_pts = frame->pts;
            continue;
        } else if (ret > 0) {
            priv->streams[i].sync_pts = frame->pts;
            ret = avformat_write_header(priv->format_ctx, NULL);
            if (ret < 0) {
                frame = moviesink_recv_dat(ctx, i);
                av_frame_free(&frame);
                goto out;
            }
        }

        frame = moviesink_recv_dat(ctx, i);

        /* user request stop, send frame which linesize = 0 */
        if (!frame->linesize[0]) {
            av_frame_free(&frame);
            ret = AVERROR_EOF;
            goto out;
        } else if (priv->state == AVMOVIE_ASYNC_STATE_PAUSED) {
            priv->streams[i].sync_pts += priv->streams[i].last_pts != AV_NOPTS_VALUE ?
                frame->pts - priv->streams[i].last_pts : 0;

            priv->streams[i].last_pts = frame->pts;
            av_frame_free(&frame);
        } else {
            ret = moviesink_write_frame(ctx, i, frame);
            priv->streams[i].last_pts = frame->pts;
            av_frame_free(&frame);
            if (ret < 0)
                goto out;
        }
    }

    ff_filter_set_ready(ctx, 100);
    return 0;

out:
    av_write_trailer(priv->format_ctx);
    moviesink_clear_dat(ctx);

    priv->state      = AVMOVIE_ASYNC_STATE_COMPLETED;
    priv->current_ms = 0;
    priv->format     = NULL;
    moviesink_send_event(ctx, AVMOVIE_ASYNC_EVENT_COMPLETED,
                            ret == AVERROR_EOF ? 0 : ret , NULL);
    moviesink_clean(ctx);
    return ret;
}

static void moviesink_stop(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret = 0;

    if (priv->state == AVMOVIE_ASYNC_STATE_STOPPED)
        return;

    ret = moviesink_send_empty_frame(ctx);
    if (ret < 0)
        av_log(ctx, AV_LOG_INFO, "moviesink_send_empty_frame ret %d, current state:%d", ret, priv->state);
    while (ret == 0) {
        ret = moviesink_proc_dat(ctx);
    }

    moviesink_clean(ctx);
}

static bool moviesink_proc_cmd(AVFilterContext *ctx, MovieSinkCmd *msg)
{
    MovieSinkPriv *priv = ctx->priv;
    bool exit = false;
    char *args;

    switch (msg->cmd) {
        case AVMOVIE_ASYNC_SET_OPTIONS:
            break;

        case AVMOVIE_ASYNC_PREPARE:
            moviesink_prepare(ctx, msg->data);
            break;

        case AVMOVIE_ASYNC_START:
            moviesink_start(ctx, msg->data);
            break;

        case AVMOVIE_ASYNC_PAUSE:
            moviesink_pause(ctx);
            break;

        case AVMOVIE_ASYNC_CLOSE:
            exit = true;
        case AVMOVIE_ASYNC_STOP:
            moviesink_stop(ctx);
            break;

        case AVMOVIE_ASYNC_PROCESS_COMMAND:
            args = strrchr(msg->data, '=');
            if (args) {
                *args++ = '\0';
                ff_filter_process_command(ctx, msg->data, args, NULL, 0, 0);
            }
            break;

        default:
            break;
    }

    av_freep(&msg);
    return exit;
}

static void *moviesink_thread(void *arg)
{
    AVFilterContext *ctx = arg;
    MovieSinkPriv *priv = ctx->priv;
    MovieSinkCmd *msg;
    bool exit = false;

    while (1) {
        pthread_mutex_lock(&priv->mutex);

        if ((msg = SIMPLEQ_FIRST(&priv->cmd_queue)) != NULL) {
            SIMPLEQ_REMOVE_HEAD(&priv->cmd_queue, entry);
            pthread_mutex_unlock(&priv->mutex);

            exit = moviesink_proc_cmd(ctx, msg);
        } else if (moviesink_dat_valid(ctx)) {
            pthread_mutex_unlock(&priv->mutex);
            moviesink_proc_dat(ctx);
        } else if (exit) {
            pthread_mutex_unlock(&priv->mutex);
            priv->state  = AVMOVIE_ASYNC_STATE_NOP;
            moviesink_send_event(ctx, AVMOVIE_ASYNC_EVENT_CLOSED, 0, NULL);
            break;
        } else {
            pthread_cond_wait(&priv->cond, &priv->mutex);
            pthread_mutex_unlock(&priv->mutex);
        }
    }

    return NULL;
}

static int moviesink_reconfig(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    bool reconfig = false;
    AVFilterLink *link;
    int64_t pts;
    int i, ret;

    if (priv->current_ms)
        return 0;

    for (i = 0; i < ctx->nb_inputs; i++) {
        link = ctx->inputs[i];

        ff_inlink_acknowledge_status(link, &ret, &pts);
        if (ret < 0) {
            reconfig = true;
            break;
        }
    }

    if (reconfig && priv->format) {
        ret = avfilter_graph_reconfig(ctx->graph, ctx);
        if (ret < 0)
            return ret;
    }

    return ret;
}

static int moviesink_activate(AVFilterContext *ctx)
{
    int i, ret = 0;
    AVFilterLink *link;
    AVFrame *frame;
    int64_t pts;

    moviesink_proc_event(ctx);

    ret = moviesink_reconfig(ctx);
    if (ret < 0)
        return ret;

    for (i = 0; i < ctx->nb_inputs; i++) {
        if (moviesink_dat_full(ctx, i))
            continue;

        link = ctx->inputs[i];
        ff_inlink_acknowledge_status(link, &ret, &pts);
        if (ret < 0) {
            moviesink_send_empty_frame(ctx);
            continue;
        }

        ret = ff_inlink_consume_frame(link, &frame);
        if (ret > 0) {
            ret = moviesink_send_dat(ctx, i, frame);
            if (ret < 0)
                av_frame_free(&frame);
        }

        if (ret < 0)
            continue;

        ff_inlink_request_frame(link);
    }

    return ret;
}

static void moviesink_uninit(AVFilterContext *ctx)
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

static int moviesink_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    MovieSinkPriv *priv = ctx->priv;
    enum AVMediaType types[] = {
        AVMEDIA_TYPE_AUDIO,
        AVMEDIA_TYPE_VIDEO,
    };
    AVFilterPad pad = { 0 };
    int i, ret, base = 0, inputs = 1;

    if (ctx->filter->name[0] == 'v')
        base = 1;
    else if (ctx->filter->name[0] != 'a')
        inputs = 2;

    priv->streams = av_calloc(inputs, sizeof(MovieStream));
    if (!priv->streams)
        return AVERROR(ENOMEM);

    SIMPLEQ_INIT(&priv->cmd_queue);
    SIMPLEQ_INIT(&priv->evt_queue);
    pthread_mutex_init(&priv->mutex, NULL);
    pthread_cond_init(&priv->cond, NULL);

    for (i = 0; i < inputs; i++) {
        priv->streams[i].type = types[base + i];
        priv->streams[i].stream_idx = -1;
        ff_framequeue_init(&priv->streams[i].dat_queue, NULL);

        pad.type = types[base + i];
        pad.name = av_asprintf("input%d", i);
        if (!pad.name) {
            ret = AVERROR(ENOMEM);
            goto out;
        }

        if ((ret = ff_append_inpad(ctx, &pad)) < 0) {
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
    moviesink_uninit(ctx);
    return ret;
}

static bool moviesink_query_audio_opts(AVFilterContext *ctx, const AVCodec *enc, const char *opt,
                                        int *value, AVChannelLayout *layout)
{
    MovieSinkPriv *priv = ctx->priv;
    int ret, n = 0, mask = 0;
    AVDictionaryEntry *tag;
    bool supported = false;

    mask = !strcmp(opt, "sample_fmt")  ? 0x01 :
           !strcmp(opt, "sample_rate") ? 0x02 :
           !strcmp(opt, "ch_layout")   ? 0x04 : 0;
    if (mask == 0)
        return false;

    if ((tag = av_dict_get(priv->format_opt, opt, NULL, 0)) == NULL)
        return false;

    if (mask & 0x01) {
        if ((ret = ff_parse_sample_format(value, tag->value, ctx)) < 0)
            return false;

        supported = enc && enc->sample_fmts ? ff_fmt_is_in(*value, enc->sample_fmts) : true;
    } else if (mask & 0x02) {
        if ((ret = ff_parse_sample_rate(value, tag->value, ctx)) < 0)
            return false;

         supported = enc && enc->supported_samplerates ?
                     ff_rate_is_in(*value, enc->supported_samplerates) : true;
    } else {
        if ((ret = ff_parse_channel_layout(layout, NULL, tag->value, ctx)) < 0)
            return false;

        if (av_channel_layout_check(layout) == 0)
            return false;

        if (enc && enc->ch_layouts) {
            while (av_channel_layout_check(&enc->ch_layouts[n])) {
                if (!av_channel_layout_compare(layout, &enc->ch_layouts[n++])) {
                    supported = true;
                    break;
                }
            }
        } else
            supported = true;
    }

    return supported;
}

static int moviesink_query_audio_fmts(AVFilterContext *ctx, int pad_id, enum AVCodecID codec_id)
{
    AVFilterLink *link = ctx->inputs[pad_id];
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterFormats *formats;
    const AVCodec *enc;

    AVChannelLayout list64[] = { { 0 }, { 0 } };
    int list[] = { 0, -1 }, *list_i32;
    bool supported;
    int n = 0, ret;

    enc = avcodec_find_encoder(codec_id);

    /* sample format */
    supported = moviesink_query_audio_opts(ctx, enc, "sample_fmt", &list[0], NULL);
    if (supported)
        formats = ff_make_format_list(list);
    else
        formats = enc && enc->sample_fmts ?
                  ff_make_sample_format_list(enc->sample_fmts) : ff_all_formats(AVMEDIA_TYPE_AUDIO);

    if (ret = ff_formats_ref(formats, &link->outcfg.formats) < 0)
        return ret;

    /* sample rate */
    supported = moviesink_query_audio_opts(ctx, enc, "sample_rate", &list[0], NULL);
    if (supported) {
        formats = ff_make_format_list(list);
    } else {
        if (enc && enc->supported_samplerates) {
            while (enc->supported_samplerates[n] != 0)
                n++;

            list_i32 = av_calloc(n + 1, sizeof(enc->supported_samplerates[0]));
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

    if (ret = ff_formats_ref(formats, &link->outcfg.samplerates) < 0)
        return ret;

    /* ch_layout */
    supported = moviesink_query_audio_opts(ctx, enc, "ch_layout", NULL, &list64[0]);
    if (supported)
        layouts = ff_make_channel_layout_list(list64);
    else
        layouts = enc && enc->ch_layouts ?
                  ff_make_channel_layout_list(enc->ch_layouts) : ff_all_channel_counts();

    if ((ret = ff_channel_layouts_ref(layouts, &link->outcfg.channel_layouts)) < 0)
        return ret;

    /* codec id */
    list[0] = codec_id;
    if (avcodec_is_pcm_lossless(codec_id))
        list[0] = AV_CODEC_ID_RAWAUDIO;

    formats = ff_make_format_list(list);
    return ff_formats_ref(formats, &link->outcfg.codecs);
}

static int moviesink_query_video_fmts(AVFilterContext *ctx, int pad_id, enum AVCodecID codec_id)
{
    AVFilterFormats *formats;
    AVFilterLink *link;
    const AVCodec *enc;
    int ret;

    int list[] = { codec_id, -1 };

    link = ctx->inputs[pad_id];
    enc  = avcodec_find_encoder(codec_id);
    if (!enc)
        return AVERROR(EINVAL);

    if (enc->pix_fmts) {
        formats = ff_make_pixel_format_list(enc->pix_fmts);
    } else {
        formats = ff_all_formats(AVMEDIA_TYPE_VIDEO);
    }

    if ((ret = ff_formats_ref(formats, &link->outcfg.formats)) < 0)
        return ret;

    formats = ff_make_format_list(list);
    return ff_formats_ref(formats, &link->outcfg.codecs);
}

static enum AVCodecID find_encoder_id(const char *name, enum AVMediaType type)
{
    const AVCodecDescriptor *desc;
    const AVCodec *codec;

    codec = avcodec_find_encoder_by_name(name);

    if (!codec && (desc = avcodec_descriptor_get_by_name(name)))
        codec = avcodec_find_encoder(desc->id);

    if (!codec || codec->type != type) {
        av_log(NULL, AV_LOG_DEBUG, "sink movie cannot find proper codec for %s.", name);
        return AV_CODEC_ID_NONE;
    }

    return codec->id;
}

static int moviesink_query_formats(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    AVDictionaryEntry *codec_opt;
    enum AVCodecID codec_id;
    AVFilterLink *link;
    int ret = 0, i;

    if (!priv->format || priv->state != AVMOVIE_ASYNC_STATE_STARTED)
        return FFERROR_NOT_READY;

    for (i = 0; ret >= 0 && i < ctx->nb_inputs; i++) {
        link = ctx->inputs[i];
        switch (link->type) {
            case AVMEDIA_TYPE_AUDIO:
                /* Codec id is configurable. */
                codec_opt = av_dict_get(priv->format_opt, "audio_codec", NULL, 0);
                if (codec_opt)
                    codec_id = find_encoder_id(codec_opt->value, AVMEDIA_TYPE_AUDIO);
                else
                    codec_id = AV_CODEC_ID_NONE;
                ret = moviesink_query_audio_fmts(ctx, i,
                    codec_id != AV_CODEC_ID_NONE ? codec_id: priv->format->audio_codec);
                break;
            case AVMEDIA_TYPE_VIDEO:
                codec_opt = av_dict_get(priv->format_opt, "video_codec", NULL, 0);
                if (codec_opt)
                    codec_id = find_encoder_id(codec_opt->value, AVMEDIA_TYPE_VIDEO);
                else
                    codec_id = AV_CODEC_ID_NONE;
                ret = moviesink_query_video_fmts(ctx, i,
                    codec_id != AV_CODEC_ID_NONE ? codec_id: priv->format->video_codec);
                break;
            default:
                break;
        }
    }

    return ret;
}

static int moviesink_process_open(AVFilterContext *ctx)
{
    MovieSinkPriv *priv = ctx->priv;
    struct sched_param param;
    pthread_attr_t attr;
    pthread_t thread;
    int ret;

    ret = moviesink_send_cmd(ctx, AVMOVIE_ASYNC_OPEN, NULL, 0);

    if (priv->state != AVMOVIE_ASYNC_STATE_NOP)
        return ret;

    priv->state = AVMOVIE_ASYNC_STATE_STOPPED;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, priv->stack_size);
#ifdef FILTER_MOVIE_PRIORITY
    priv->priority = FILTER_MOVIE_PRIORITY;
#endif
    param.sched_priority = priv->priority;
    pthread_attr_setschedparam(&attr, &param);
    ret = pthread_create(&thread, &attr, moviesink_thread, ctx);
    if (ret != 0) {
        priv->state = AVMOVIE_ASYNC_STATE_NOP;
        return AVERROR(ret);
    }

    pthread_setname_np(thread, ctx->name);
    pthread_detach(thread);
    return 0;
}

static int moviesink_process_prepare(AVFilterContext *ctx, const char *args)
{
    return moviesink_send_cmd(ctx, AVMOVIE_ASYNC_PREPARE, args, strlen(args) + 1);
}

static int moviesink_process_start(AVFilterContext *ctx)
{
    return moviesink_send_cmd(ctx, AVMOVIE_ASYNC_START, NULL, 0);
}

static int moviesink_process_pause(AVFilterContext *ctx)
{
    return moviesink_send_cmd(ctx, AVMOVIE_ASYNC_PAUSE, NULL, 0);
}

static int moviesink_process_quit(AVFilterContext *ctx, const char *cmd)
{
    MovieSinkPriv *priv = ctx->priv;
    bool reset, close;
    MovieSinkCmd *msg;
    int ret = 0;

    reset = !strcmp(cmd, "reset");
    close = !strcmp(cmd, "close");

    if (reset || close) {
        pthread_mutex_lock(&priv->mutex);
        while ((msg = SIMPLEQ_FIRST(&priv->cmd_queue)) != NULL) {
            SIMPLEQ_REMOVE_HEAD(&priv->cmd_queue, entry);
            av_freep(&msg);
        }
        pthread_mutex_unlock(&priv->mutex);
    }

    moviesink_clear_dat(ctx);

    if (close)
        ret = moviesink_send_cmd(ctx, AVMOVIE_ASYNC_CLOSE, NULL, 0);
    else
        ret = moviesink_send_cmd(ctx, AVMOVIE_ASYNC_STOP, NULL, 0);

    if (priv->format_opt)
        av_dict_free(&priv->format_opt);

    return ret;
}

static int moviesink_process_process_command(AVFilterContext *ctx, const char *cmd, const char *args)
{
    int len, ret;
    char *ptr;

    if (!cmd || !args)
        return AVERROR(EINVAL);

    len = strlen(cmd) + (args ? strlen(args) : 1) + 2;
    ptr = av_malloc(len);
    if (!ptr)
        return AVERROR(ENOMEM);

    snprintf(ptr, len, "%s=%s", cmd, args);
    ret = moviesink_send_cmd(ctx, AVMOVIE_ASYNC_PROCESS_COMMAND, ptr, len);
    free(ptr);

    return ret;
}

static int moviesink_get_position(AVFilterContext *ctx, char *res, int res_len)
{
    MovieSinkPriv *priv = ctx->priv;

    if (!res || !res_len)
        return AVERROR(EINVAL);

    snprintf(res, res_len, "%u", priv->current_ms);
    return 0;
}

static int moviesink_process_dump(AVFilterContext *ctx, char *res, int res_len)
{
    MovieSinkPriv *priv = ctx->priv;

    snprintf(res, res_len, "st: %d", priv->state);

    return 0;
}

static int moviesink_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                      char *res, int res_len, int flags)
{
    MovieSinkPriv *priv = ctx->priv;
    struct AVMovieAsyncEventCookie *event;
    int ret;

    if (!strcmp(cmd, "open")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s open.\n", __func__, ctx->name);
        return moviesink_process_open(ctx);
    } else if (!strcmp(cmd, "set_event")) {
        event = (AVMovieAsyncEventCookie *)args;
        priv->event  = event->event;
        priv->cookie = event->cookie;
        return 0;
    } else if (!strcmp(cmd, "set_options")) {
        if (!args)
            return AVERROR(EINVAL);

        pthread_mutex_lock(&priv->mutex);
        ret = av_dict_parse_string(&priv->format_opt, args, "=", ":", 0);
        pthread_mutex_unlock(&priv->mutex);
        return ret;
    } else if (!strcmp(cmd, "get_options")) {
        AVDictionary **dst = (AVDictionary **)res;

        pthread_mutex_lock(&priv->mutex);
        ret = av_dict_copy(dst, priv->format_opt, 0);
        pthread_mutex_unlock(&priv->mutex);
        if (priv->format && priv->format->flags & AVFMT_GLOBALHEADER)
            av_dict_set_int(dst, "flags", AV_CODEC_FLAG_GLOBAL_HEADER, 0);
        return ret;
    } else if (!strcmp(cmd, "prepare")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s prepare %s.\n", __func__, ctx->name, args);
        return moviesink_process_prepare(ctx, args);
    } else if (!strcmp(cmd, "start")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s start.\n", __func__, ctx->name);
        return moviesink_process_start(ctx);
    } else if (!strcmp(cmd, "pause")) {
        return moviesink_process_pause(ctx);
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "reset") || !strcmp(cmd, "close")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s %s. pos %d\n", __func__, ctx->name, cmd, priv->current_ms);
        return moviesink_process_quit(ctx, cmd);
    } else if (!strcmp(cmd, "get_position")) {
        return moviesink_get_position(ctx, res, res_len);
    } else if (!strcmp(cmd, "dump")) {
        return moviesink_process_dump(ctx, res, res_len);
    } else if (!res && !res_len) {
        return moviesink_process_process_command(ctx, cmd, args);
    } else {
        return AVERROR(ENOSYS);
    }
}

#define OFFSET(x) offsetof(MovieSinkPriv, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption moviesink_async_options[] = {
    { "datqmax",    "maximum number of dat queue", OFFSET(dat_max),    AV_OPT_TYPE_INT,    {.i64 = 4 },      2, 8,         FLAGS },
    { "cmdqmax",    "maximum number of cmd queue", OFFSET(cmd_max),    AV_OPT_TYPE_INT,    {.i64 = 16 },     8, 32,        FLAGS },
    { "stack_size", "stack size of work thread",   OFFSET(stack_size), AV_OPT_TYPE_INT,    {.i64 = 61440 },  0, INT32_MAX, FLAGS },
    { "priority",   "priority of work thread",     OFFSET(priority),   AV_OPT_TYPE_INT,    {.i64 = 244 },    0, INT16_MAX, FLAGS },
    { NULL },
};

static const struct AVClass *moviesink_child_class_iterate(void **iter)
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

static void *moviesink_child_next(void *obj, void *prev)
{
    MovieSinkPriv *priv = obj;

    if (!prev)
        return priv->format_ctx;
    else
        return NULL;
}

#if CONFIG_AMOVIESINK_ASYNC_FILTER

static const AVClass amoviesink_async_class = {
    .class_name          = "amoviesink_async_class",
    .item_name           = av_default_item_name,
    .option              = moviesink_async_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = moviesink_child_next,
    .child_class_iterate = moviesink_child_class_iterate,
};

const AVFilter ff_sink_amoviesink_async = {
    .name            = "amoviesink_async",
    .description     = NULL_IF_CONFIG_SMALL("movie sink asyncchronously, end of the filter graph."),
    .priv_class      = &amoviesink_async_class,
    .priv_size       = sizeof(MovieSinkPriv),
    .init_dict       = moviesink_init_dict,
    .uninit          = moviesink_uninit,
    FILTER_QUERY_FUNC(moviesink_query_formats),
    .activate        = moviesink_activate,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .process_command = moviesink_process_command,
};
#endif

#if CONFIG_MOVIESINK_ASYNC_FILTER

static const AVClass moviesink_async_class = {
    .class_name          = "moviesink_async_class",
    .item_name           = av_default_item_name,
    .option              = moviesink_async_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = moviesink_child_next,
    .child_class_iterate = moviesink_child_class_iterate,
};

const AVFilter ff_sink_moviesink_async = {
    .name            = "moviesink_async",
    .description     = NULL_IF_CONFIG_SMALL("movie sink asyncchronously, end of the filter graph."),
    .priv_class      = &moviesink_async_class,
    .priv_size       = sizeof(MovieSinkPriv),
    .init_dict       = moviesink_init_dict,
    .uninit          = moviesink_uninit,
    FILTER_QUERY_FUNC(moviesink_query_formats),
    .activate        = moviesink_activate,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .process_command = moviesink_process_command,
};
#endif

#if CONFIG_VMOVIESINK_ASYNC_FILTER

static const AVClass vmoviesink_async_class = {
    .class_name          = "vmoviesink_async_class",
    .item_name           = av_default_item_name,
    .option              = moviesink_async_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = moviesink_child_next,
    .child_class_iterate = moviesink_child_class_iterate,
};

const AVFilter ff_sink_vmoviesink_async = {
    .name            = "vmoviesink_async",
    .description     = NULL_IF_CONFIG_SMALL("video sink asynchronously, end of the filter graph."),
    .priv_class      = &vmoviesink_async_class,
    .priv_size       = sizeof(MovieSinkPriv),
    .init_dict       = moviesink_init_dict,
    .uninit          = moviesink_uninit,
    FILTER_QUERY_FUNC(moviesink_query_formats),
    .activate        = moviesink_activate,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_INPUTS,
    .process_command = moviesink_process_command,
};
#endif
