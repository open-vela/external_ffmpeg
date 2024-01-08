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
 * movie source asynchronously
 *
 */

#include "config_components.h"

#include <float.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <sys/queue.h>

#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavcodec/codec_par.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"

#include "packet_wrapper.h"
#include "movie_async.h"
#include "filters.h"

#define AVMOVIE_ASYNC_CMD_QUEUE_IDX           (1 << 0)
#define AVMOVIE_ASYNC_DATA_QUEUE_IDX          (1 << 1)

typedef struct MovieCmd {
    SIMPLEQ_ENTRY(MovieCmd) entry;
    int                     cmd;
    char                    data[0];
} MovieCmd;

typedef struct MovieEvent {
    SIMPLEQ_ENTRY(MovieEvent) entry;
    int                       event;
    int                       ret;
    char                      *extra;
    char                      data[0];
} MovieEvent;

SIMPLEQ_HEAD(MovieCmdQueue, MovieCmd);
SIMPLEQ_HEAD(MovieEvtQueue, MovieEvent);

typedef struct MovieStream {
    enum AVMediaType  type;
    int               index;                     /**< AVStream index of AVFormatContext */
    FFFrameQueue      dat_queue;
    AVRational        time_base;
    AVRational        frame_rate;
    int64_t           start_time;
    bool              completed;
    AVCodecParameters *codecpar;
} MovieStream;

typedef struct MovieAsyncContext {
    /* common A/V fields */
    const                     AVClass *class;

    int                       dat_max;
    int                       dat_cnt;
    int                       cmd_max;
    int                       stack_size;
    int                       priority;
    char                      *protocol_map;

    MovieStream               *streams;       /**< array of all streams, one per output */
    AVFormatContext           *format_ctx;
    AVDictionary              *format_opt;
    AVDictionary              *global_opts;

    pthread_mutex_t           mutex;
    pthread_cond_t            cond;

    struct MovieCmdQueue      cmd_queue;     /**< cmd queue which mediad to worker thread */
    struct MovieEvtQueue      evt_queue;     /**< event queue which worker thread to mediad */

    int                       state;
    bool                      eof_reached;
    int                       live_stream;

    unsigned                  current_ms;     /** < current timestamp of the decoded frame */
    unsigned                  lastseek_ms;    /** < timestamp of seek, only used in offload */
    unsigned                  duration_ms;    /** < duration of whole stream */
    int                       loop_count;
    int                       pending_stop;
    bool                      frame_sent;
    bool                      need_reconfig;

    void                      *cookie;
    av_movie_async_event_func event;
} MovieAsyncContext;

#define OFFSET(x) offsetof(MovieAsyncContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption movie_async_options[]= {
    { "datqmax",      "maximum number of dat queue",        OFFSET(dat_max),      AV_OPT_TYPE_INT,    {.i64 = 4 },     1, INT_MAX, FLAGS },
    { "datqcnt",      "prebuff frame count before playing", OFFSET(dat_cnt),      AV_OPT_TYPE_INT,    {.i64 = 1 },     0, INT_MAX, FLAGS },
    { "cmdqmax",      "maximum number of cmd queue",        OFFSET(cmd_max),      AV_OPT_TYPE_INT,    {.i64 = 16 },    8, 32,      FLAGS },
    { "stack_size",   "stack size of work thread",          OFFSET(stack_size),   AV_OPT_TYPE_INT,    {.i64 = 61440 }, 0, INT_MAX, FLAGS },
    { "priority",     "priority of work thread",            OFFSET(priority),     AV_OPT_TYPE_INT,    {.i64 = 244 },   0, INT_MAX, FLAGS },
    { "protocol_map", "mapping of protocol",                OFFSET(protocol_map), AV_OPT_TYPE_STRING, {.str = NULL},   0, 0,       FLAGS },
    { "live_stream",  "realtime stream mode",               OFFSET(live_stream),  AV_OPT_TYPE_BOOL,   {.i64 = 0},      0, 1,       FLAGS },
    { NULL },
};

static inline void movie_async_notify_event(MovieAsyncContext *movie, int event, int ret, const char *extra)
{
    if (movie->event != NULL && movie->cookie != NULL) {
        movie->event(movie->cookie, event, ret, extra);

        if (event == AVMOVIE_ASYNC_EVENT_CLOSED) {
            movie->event  = NULL;
            movie->cookie = NULL;
        }
    }
}

static int movie_async_send_cmd(AVFilterContext *ctx, const int cmd, const void *data, size_t size)
{
    MovieAsyncContext *movie = ctx->priv;
    MovieCmd *msg, *tmp;
    int cnt = 0;

    msg = av_malloc(sizeof(MovieCmd) + size);
    if (!msg)
        return AVERROR(ENOMEM);

    msg->cmd = cmd;

    if (data && size)
        memcpy(msg->data, data, size);

    pthread_mutex_lock(&movie->mutex);

    SIMPLEQ_FOREACH(tmp, &movie->cmd_queue, entry) cnt++;
    if (cnt >= movie->cmd_max && msg->cmd < AVMOVIE_ASYNC_STOP) {
        pthread_mutex_unlock(&movie->mutex);
        av_freep(&msg);

        return AVERROR(ENOMEM);
    }

    SIMPLEQ_INSERT_TAIL(&movie->cmd_queue, msg, entry);
    pthread_cond_signal(&movie->cond);
    pthread_mutex_unlock(&movie->mutex);

    return 0;
}

static int movie_async_send_dat(AVFilterContext *ctx, int pad_id, AVFrame *frame)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret;

    pthread_mutex_lock(&movie->mutex);
    ret = ff_framequeue_add(&movie->streams[pad_id].dat_queue, frame);
    pthread_mutex_unlock(&movie->mutex);

    ff_filter_set_ready(ctx, 100);

    return ret;
}

static int movie_async_send_event(AVFilterContext *ctx, int event, int ret, const char *extra)
{
    MovieAsyncContext *movie = ctx->priv;
    MovieEvent *evt;

    if (extra)
        evt = av_malloc(sizeof(MovieEvent) + strlen(extra) + 1);
    else
        evt = av_malloc(sizeof(MovieEvent));
    if (!evt)
        return AVERROR(ENOMEM);

    evt->event = event;
    evt->ret   = ret;
    evt->extra = NULL;

    if (extra) {
        evt->extra = evt->data;
        strcpy(evt->extra, extra);
    }

    pthread_mutex_lock(&movie->mutex);
    SIMPLEQ_INSERT_TAIL(&movie->evt_queue, evt, entry);
    pthread_mutex_unlock(&movie->mutex);

    ff_filter_set_ready(ctx, 100);
    return 0;
}

static bool movie_async_peek_info(AVFilterContext *ctx, int pad_id, AVCodecParameters **dst)
{
    MovieAsyncContext *movie = ctx->priv;
    AVCodecParameters *par = NULL;
    AVFrame *frame = NULL;
    bool ret = false;

    pthread_mutex_lock(&movie->mutex);
    if (ff_framequeue_queued_frames(&movie->streams[pad_id].dat_queue))
        frame = ff_framequeue_peek(&movie->streams[pad_id].dat_queue, 0);

    if (frame && frame->opaque_ref && movie->streams[pad_id].codecpar == NULL) {
        par = avcodec_parameters_alloc();
        if (!par)
            goto out;

        if (avcodec_parameters_copy(par, (AVCodecParameters*)frame->opaque_ref->data) < 0) {
            avcodec_parameters_free(&par);
            goto out;
        }

        movie->streams[pad_id].codecpar = par;
    }

    if (frame && movie->streams[pad_id].codecpar) {
        *dst = movie->streams[pad_id].codecpar;
        ret = true;
    }

out:
    pthread_mutex_unlock(&movie->mutex);
    return ret;
}

static void movie_async_drop_dat(AVFilterContext *ctx, int pad_id)
{
    MovieAsyncContext *movie = ctx->priv;

    pthread_mutex_lock(&movie->mutex);
    if (ff_framequeue_queued_frames(&movie->streams[pad_id].dat_queue) > 1) {
        AVFrame *frame = ff_framequeue_take(&movie->streams[pad_id].dat_queue);
        if (frame->opaque_ref) {
            AVFrame *peek_frame = ff_framequeue_peek(&movie->streams[pad_id].dat_queue, 0);
            if (peek_frame->opaque_ref)
                av_buffer_unref(&peek_frame->opaque_ref);
            peek_frame->opaque_ref = frame->opaque_ref;
            frame->opaque_ref = NULL;
        }

        av_log(ctx, AV_LOG_WARNING, "drop a %s frame, pts %" PRId64 ", size %d.",
               (movie->streams[pad_id].type == AVMEDIA_TYPE_AUDIO) ? "audio" : "video",
               frame->pts, frame->linesize[0]);

        av_frame_free(&frame);
    }

    pthread_mutex_unlock(&movie->mutex);
}

static AVFrame *movie_async_recv_dat(AVFilterContext *ctx, int pad_id)
{
    MovieAsyncContext *movie = ctx->priv;
    AVFrame *frame = NULL;

    pthread_mutex_lock(&movie->mutex);
    if (ff_framequeue_queued_frames(&movie->streams[pad_id].dat_queue)) {
        frame = ff_framequeue_take(&movie->streams[pad_id].dat_queue);
        pthread_cond_signal(&movie->cond);
    }

    pthread_mutex_unlock(&movie->mutex);
    return frame;
}

static int movie_async_dat_count(AVFilterContext *ctx, int pad_id)
{
    MovieAsyncContext *movie = ctx->priv;
    int count = 0;

    if (movie->state != AVMOVIE_ASYNC_STATE_STARTED)
        return count;

    pthread_mutex_lock(&movie->mutex);
    count = ff_framequeue_queued_frames(&movie->streams[pad_id].dat_queue);
    pthread_mutex_unlock(&movie->mutex);

    return count;
}

static int movie_async_interrupt(void *opaque)
{
    AVFilterContext *ctx = opaque;
    MovieAsyncContext *movie = ctx->priv;
    MovieCmd *msg;
    int interrupt = 0;

    pthread_mutex_lock(&movie->mutex);
    SIMPLEQ_FOREACH(msg, &movie->cmd_queue, entry) {
        if (msg->cmd >= AVMOVIE_ASYNC_STOP) {
            interrupt = 1;
            break;
        }
    }

    pthread_mutex_unlock(&movie->mutex);
    return interrupt;
}

static void movie_async_clear_queue(AVFilterContext *ctx, int what)
{
    MovieAsyncContext *movie = ctx->priv;
    AVFrame *frame;
    MovieCmd *msg;
    int i;

    pthread_mutex_lock(&movie->mutex);
    if (what & AVMOVIE_ASYNC_CMD_QUEUE_IDX) {
        while ((msg = SIMPLEQ_FIRST(&movie->cmd_queue)) != NULL) {
            SIMPLEQ_REMOVE_HEAD(&movie->cmd_queue, entry);
            av_freep(&msg);
        }
    }

    if (what & AVMOVIE_ASYNC_DATA_QUEUE_IDX) {
        for (i = 0; i < ctx->nb_outputs; i++) {
            while (ff_framequeue_queued_frames(&movie->streams[i].dat_queue)) {
                frame = ff_framequeue_take(&movie->streams[i].dat_queue);
                av_frame_free(&frame);
            }
        }
    }
    pthread_mutex_unlock(&movie->mutex);
}

static bool movie_async_dat_available(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int i;

    if (movie->state >= AVMOVIE_ASYNC_STATE_STOPPED)
        return false;

    if (movie->live_stream)
        return true;

    /* As long as one data queue less than movie->dat_max, continue read */
    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie->streams[i].index < 0)
            continue;

        if (ff_framequeue_queued_frames(&movie->streams[i].dat_queue) < movie->dat_max)
            return true;
    }

    return false;
}

static void movie_async_close_demuxer(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    MovieStream *stream;
    int i;

    if (movie->format_ctx)
        avformat_close_input(&movie->format_ctx);

    if (movie->format_opt) {
        AVDictionaryEntry *entry;
        while ((entry = av_dict_get(movie->format_opt, "", entry, AV_DICT_IGNORE_SUFFIX))) {
            AVDictionaryEntry *tmp_entry = av_dict_get(movie->global_opts, entry->key, NULL, 0);
            if (tmp_entry && strcmp(tmp_entry->value, entry->value) == 0) {
                av_dict_set(&movie->global_opts, entry->key, NULL, 0);
            }
        }

        av_dict_free(&movie->format_opt);
    }

    movie->current_ms = 0;
    movie->lastseek_ms = 0;
}

static int movie_async_seek(AVFilterContext *ctx, unsigned ms, bool flush)
{
    MovieAsyncContext *movie = ctx->priv;
    uint64_t timestamp = ms * 1000LL;
    int i, ret = AVERROR(EPERM);

    if (!movie->format_ctx)
        goto end;

    if (movie->format_ctx->start_time != AV_NOPTS_VALUE)
        timestamp += movie->format_ctx->start_time;

    ret = avformat_seek_file(movie->format_ctx, -1, INT64_MIN, timestamp, INT64_MAX, 0);
    if (ret < 0)
        goto end;

    if (flush)
        movie_async_clear_queue(ctx, AVMOVIE_ASYNC_DATA_QUEUE_IDX);

    movie->current_ms = ms;
    movie->lastseek_ms = ms;

    movie->eof_reached = false;
    for (i = 0; i < ctx->nb_outputs; i++)
        movie->streams[i].completed = false;
end:
    movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_SEEKED, ret, NULL);
    return ret;
}

static void movie_async_map_protocol(AVFilterContext *ctx, const char *url, char *dst, int length)
{
    MovieAsyncContext *movie = ctx->priv;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *tag;
    char proto[128];

    av_url_split(proto, sizeof(proto), NULL, 0, NULL, 0, NULL, NULL, 0, url);

    if (movie->protocol_map && proto[0]) {
        av_dict_parse_string(&opts, movie->protocol_map, ">", "|", 0);
        if ((tag = av_dict_get(opts, proto, NULL, 0))) {
            snprintf(dst, length, "%s:%s", tag->value, url);
            av_dict_free(&opts);
            return ;
        }

        av_dict_free(&opts);
    }

    av_strlcpy(dst, url, length);
}

static int movie_async_open_demuxer(AVFilterContext *ctx, const char *filename)
{
    MovieAsyncContext *movie = ctx->priv;
    const AVInputFormat *iformat = NULL;
    char name[MAX_URL_SIZE];
    unsigned seek_point = 0;
    AVDictionaryEntry *tag;
    AVStream *stream;
    int ret, i;

    if ((tag = av_dict_get(movie->format_opt, "format", NULL, 0))) {
        iformat = av_find_input_format(tag->value);
        if (!iformat)
            return AVERROR(EINVAL);
    }

    movie->format_ctx = avformat_alloc_context();
    if (!movie->format_ctx)
        return AVERROR(ENOMEM);

    movie->eof_reached = false;

    movie->format_ctx->interrupt_callback.callback = movie_async_interrupt;
    movie->format_ctx->interrupt_callback.opaque = ctx;
    movie->format_ctx->flags |= AVFMT_FLAG_FAST_SEEK;

    if (movie->global_opts)
        av_dict_copy(&movie->format_opt, movie->global_opts, 0);

    movie_async_map_protocol(ctx, filename, name, sizeof(name));

    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s start open input.\n", name);
    ret = avformat_open_input(&movie->format_ctx, name, iformat, &movie->format_opt);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "Failed to avformat_open_input ret %d, %s.\n", ret, av_err2str(ret));
        goto out;
    }

    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s open input done.\n", name);

    ret = avformat_find_stream_info(movie->format_ctx, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "Failed to find stream info ret %d, %s.\n", ret, av_err2str(ret));
        goto out;
    }

    for (i = 0; i < movie->format_ctx->nb_streams; i++)
        movie->format_ctx->streams[i]->discard = AVDISCARD_ALL;

    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s find stream info done.\n", name);

    for (i = 0; i < ctx->nb_outputs; i++) {
        ret = av_find_best_stream(movie->format_ctx, movie->streams[i].type, -1, -1, NULL, 0);
        if (ret < 0) {
            if (ctx->nb_outputs > 1 && movie->streams[i].type == AVMEDIA_TYPE_AUDIO) {
                movie->streams[i].index = -1;
                continue;
            } else {
                av_log(ctx, AV_LOG_WARNING, "Failed to find best stream ret %d %s.\n", ret, av_err2str(ret));
                goto out;
            }
        }
        stream = movie->format_ctx->streams[ret];

        stream->discard              = AVDISCARD_DEFAULT;
        movie->streams[i].index      = stream->index;
        movie->streams[i].time_base  = stream->time_base;
        movie->streams[i].frame_rate = stream->r_frame_rate;
        movie->streams[i].start_time = av_rescale_q(movie->format_ctx->start_time,
                                                    AV_TIME_BASE_Q, stream->time_base);
        movie->streams[i].completed  = false;
    }
    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s open decode DONE start_time:%" PRId64 ".\n", name, movie->format_ctx->start_time);

    if (movie->format_ctx->duration == AV_NOPTS_VALUE)
        movie->duration_ms = 0;
    else
        movie->duration_ms = av_rescale(movie->format_ctx->duration,
                                        1000, AV_TIME_BASE);

    /* do seek if requested */
    if ((tag = av_dict_get(movie->format_opt, "seek_point", NULL, 0))) {
        seek_point = strtoul(tag->value, NULL, 0);
    }

    if (seek_point > 0)
        movie_async_seek(ctx, seek_point, false);

    return 0;

out:
    movie_async_close_demuxer(ctx);
    return ret;
}

static void movie_async_prepare(AVFilterContext *ctx, const char *filename)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret = AVERROR(EPERM);

    if (movie->state != AVMOVIE_ASYNC_STATE_STOPPED)
        goto out;

    ret = movie_async_open_demuxer(ctx, filename);
    if (ret >= 0)
        movie->state = AVMOVIE_ASYNC_STATE_PREPARED;
    else if (movie_async_interrupt(ctx))
        ret = AVERROR(ECANCELED);

out:
    movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_PREPARED, ret, NULL);
}

static void movie_async_start(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret = AVERROR(EPERM);

    if (movie->state == AVMOVIE_ASYNC_STATE_PREPARED ||
        movie->state == AVMOVIE_ASYNC_STATE_PAUSED ||
        movie->state == AVMOVIE_ASYNC_STATE_COMPLETED) {
        movie->state = AVMOVIE_ASYNC_STATE_STARTED;
        ret = 0;
    }

    movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_STARTED, ret, NULL);
}

static void movie_async_pause(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret = AVERROR(EPERM);

    if (movie->state == AVMOVIE_ASYNC_STATE_STARTED) {
        movie->state = AVMOVIE_ASYNC_STATE_PAUSED;
        ret = 0;
    }

    movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_PAUSED, ret, NULL);
}

static void movie_async_stop(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;

    if (movie->state == AVMOVIE_ASYNC_STATE_STOPPED)
        return;

    movie_async_clear_queue(ctx, AVMOVIE_ASYNC_DATA_QUEUE_IDX);
    movie_async_close_demuxer(ctx);
    movie->state = AVMOVIE_ASYNC_STATE_STOPPED;
    movie->pending_stop = 0;
    movie->loop_count = 0;
    movie->frame_sent = false;

    movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_STOPPED, 0, NULL);
}

static int movie_async_send_frame(AVFilterContext *ctx, AVPacket *pkt, int pad_id)
{
    MovieAsyncContext *movie = ctx->priv;
    AVCodecParameters *src, *dst = NULL;
    AVFrame *frame = NULL;
    AVDictionary *dict = NULL;
    int ret;

    src = movie->format_ctx->streams[pkt->stream_index]->codecpar;
    if (!movie->frame_sent) {
        dst = avcodec_parameters_alloc();
        if (!dst)
            return AVERROR(ENOMEM);

        ret = avcodec_parameters_copy(dst, src);
        if (ret < 0)
            goto out;

        dict = movie->format_opt;
    }

    frame = wrap_frame(pkt, dst, dict);
    if (!frame)
        goto out;

    frame->format = src->format;
    frame->sample_rate = src->sample_rate;
    frame->nb_samples  = src->frame_size;
    av_channel_layout_copy(&frame->ch_layout, &src->ch_layout);

    ret = movie_async_send_dat(ctx, pad_id, frame);
    if (ret < 0)
        goto out;

    return ret;

out:
    avcodec_parameters_free(&dst);
    av_frame_free(&frame);
    return ret;
}

static int movie_async_read_frame(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    AVPacket *pkt;
    int i, ret;

    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);

    /* read a new packet from input stream */
    ret = av_read_frame(movie->format_ctx, pkt);
    if (ret < 0)
        goto out;

    /* send the packet to its decoder, if any */
    for (i = 0; i < ctx->nb_outputs; i++) {
        MovieStream *stream = &movie->streams[i];
        if (pkt->stream_index == stream->index) {
            movie->current_ms = av_rescale_q(pkt->pts + pkt->duration, stream->time_base, av_make_q(1, 1000));
            pkt->pts -= stream->start_time;
            ret = movie_async_send_frame(ctx, pkt, i);
            if (ret < 0)
                goto out;

            /* assign to NULL when the type of AVPacket matched with outputs pad.
             * otherwise free AVPacket */
            pkt = NULL;
            break;
        }
    }

    /* drop frame if the number of data queue exceeds max data number.
     * only drop audio frames. */
    if (movie->live_stream) {
        for (i = 0; i < ctx->nb_outputs; i++) {
            if (movie->streams[i].type == AVMEDIA_TYPE_AUDIO &&
                movie_async_dat_count(ctx, i) > movie->dat_max) {
                movie_async_drop_dat(ctx, i);
            }
        }
    }

out:
    av_packet_free(&pkt);
    return ret;
}

static int movie_async_loop(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret = AVERROR_EOF;

    if (movie->loop_count) {
        ret = movie_async_seek(ctx, 0, true);
        movie->loop_count -= movie->loop_count > 0;
    }

    return ret;
}

static void movie_async_completed(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret;

    movie->state = AVMOVIE_ASYNC_STATE_COMPLETED;
    ret = movie_async_loop(ctx);
    if (ret < 0) {
        av_log(ctx, AV_LOG_INFO, "%s rcv completed.\n", ctx->name);
        movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_COMPLETED, 0, NULL);
    } else {
        av_log(ctx, AV_LOG_INFO, "%s loop %d.\n", ctx->name, movie->loop_count);
        movie_async_send_cmd(ctx, AVMOVIE_ASYNC_START, NULL, 0);
    }
}

static int movie_async_send_vsyncmode(AVFilterContext *ctx, int audio_alive)
{
    MovieAsyncContext *movie = ctx->priv;
    const char *mode;
    int i, ret;

    for (i = 0; i < ctx->nb_outputs; i++)
        if (movie->streams[i].type == AVMEDIA_TYPE_VIDEO) {
            if (movie->live_stream)
                mode = "bypass";
            else
                mode = audio_alive ? "audio" : "system";

            ret = avfilter_forward_command(ctx, i, NULL, "syncmode", mode, NULL, 0, 0);
            if (ret < 0)
                av_log(ctx, AV_LOG_ERROR, "Failed to set syncmode:%s ret %d, %s.\n", mode, ret, av_err2str(ret));
        }

    return 0;
}

static void movie_async_proc_event(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int i;

    while (1) {
        MovieEvent *event;

        pthread_mutex_lock(&movie->mutex);
        if ((event = SIMPLEQ_FIRST(&movie->evt_queue)) != NULL)
            SIMPLEQ_REMOVE_HEAD(&movie->evt_queue, entry);
        pthread_mutex_unlock(&movie->mutex);

        if (!event)
            break;

        if (event->ret < 0)
            goto notify;

        switch (event->event) {
            case AVMOVIE_ASYNC_EVENT_STARTED:
                for (i = 0; i < ctx->nb_outputs; i++)
                    avfilter_forward_command(ctx, i, NULL, "play", NULL, NULL, 0, 0);

                movie_async_send_vsyncmode(ctx, movie->streams[0].type == AVMEDIA_TYPE_AUDIO &&
                                           movie->streams[0].index >= 0);
                movie->need_reconfig = true;
                break;

            case AVMOVIE_ASYNC_EVENT_PAUSED:
                for (i = 0; i < ctx->nb_outputs; i++)
                    avfilter_forward_command(ctx, i, NULL, "pause", NULL, NULL, 0, 0);
                break;

            case AVMOVIE_ASYNC_EVENT_SEEKED:
                for (i = 0; i < ctx->nb_outputs; i++)
                    avfilter_forward_command(ctx, i, NULL, "flush", NULL, NULL, 0, 0);
                break;

            case AVMOVIE_ASYNC_EVENT_STOPPED:
                for (i = 0; i < ctx->nb_outputs; i++) {
                    if (!ff_outlink_get_status(ctx->outputs[i])) {
                        avfilter_forward_command(ctx, i, NULL, "flush", "eos", NULL, 0, 0);
                        ff_avfilter_link_set_in_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
                    }
                    avcodec_parameters_free(&movie->streams[i].codecpar);
                }
                break;
        }

notify:
        movie_async_notify_event(movie, event->event, event->ret, event->extra);
        av_freep(&event);
    }
}

static int movie_async_send_eos_frame(AVFilterContext *ctx, int pad_id)
{
    MovieAsyncContext *movie = ctx->priv;
    AVPacket *pkt;
    int ret;

    pkt = av_packet_alloc();
    if (!pkt)
        return AVERROR(ENOMEM);

    pkt->flags        = AV_PKT_FLAG_EVT_EOS;
    pkt->stream_index = movie->streams[pad_id].index;

    ret = movie_async_send_frame(ctx, pkt, pad_id);
    if (ret < 0) {
        av_packet_free(&pkt);
        return ret;
    }

    return 0;
}

static bool movie_async_proc_dat(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret, i;

    ret = movie_async_read_frame(ctx);
    if (ret >= 0 || ret == AVERROR_EXIT)
        return false;

    movie->eof_reached = true;
    if (ret != AVERROR_EOF) {
        av_log(ctx, AV_LOG_ERROR, "Failed read frame ret,%d,%s.\n", ret, av_err2str(ret));
        movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_COMPLETED, ret, NULL);
        return false;
    }

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie->streams[i].index < 0)
            continue;

        ret = movie_async_send_eos_frame(ctx, i);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed outputs %d/%d send eos frame ret,%d,%s.\n",
                i, ctx->nb_outputs, ret, av_err2str(ret));
            break;
        }
    }

    if (!movie->pending_stop)
        return false;

    return true;
}

static bool movie_async_proc_cmd(AVFilterContext *ctx, MovieCmd *msg)
{
    MovieAsyncContext *movie = ctx->priv;
    AVMovieAsyncEventCookie *event;
    unsigned time, pending_stop;
    bool exit = false;
    char *args;

    switch (msg->cmd) {
        case AVMOVIE_ASYNC_SET_OPTIONS:
            av_dict_parse_string(&movie->format_opt, msg->data, "=", ":", 0);
            break;

        case AVMOVIE_ASYNC_SET_LOOP:
            movie->loop_count = strtol(msg->data, NULL, 0);
            break;

        case AVMOVIE_ASYNC_PREPARE:
            movie_async_prepare(ctx, msg->data);
            break;

        case AVMOVIE_ASYNC_START:
            movie_async_start(ctx);
            break;

        case AVMOVIE_ASYNC_PAUSE:
            movie_async_pause(ctx);
            break;

        case AVMOVIE_ASYNC_SEEK:
            time = strtoul(msg->data, NULL, 0);
            movie_async_seek(ctx, time, true);
            break;

        case AVMOVIE_ASYNC_CLOSE:
            pending_stop = strtoul(msg->data, NULL, 0);
            if (pending_stop && movie->state < AVMOVIE_ASYNC_STATE_STOPPED) {
                movie->pending_stop = pending_stop;
                break;
            }

            exit = true;
        case AVMOVIE_ASYNC_STOP:
        case AVMOVIE_ASYNC_RESET:
            movie_async_stop(ctx);
            break;

        case AVMOVIE_ASYNC_PROCESS_COMMAND:
            args = strrchr(msg->data, '=');
            *args++ = '\0';

            ff_filter_process_command(ctx, msg->data, args, NULL, 0, 0);
            break;

        case AVMOVIE_ASYNC_COMPLETED:
            movie_async_completed(ctx);
            break;

        default:
            break;
    }

    av_freep(&msg);
    return exit;
}

static void *movie_async_thread(void *arg)
{
    AVFilterContext *ctx = arg;
    MovieAsyncContext *movie = ctx->priv;
    bool exit = false;
    MovieCmd *msg;

    while (1) {
        pthread_mutex_lock(&movie->mutex);
        if ((msg = SIMPLEQ_FIRST(&movie->cmd_queue)) != NULL) {
            SIMPLEQ_REMOVE_HEAD(&movie->cmd_queue, entry);
            pthread_mutex_unlock(&movie->mutex);

            /* close flag shouldnot be affected by following complete cmd. */
            if (movie_async_proc_cmd(ctx, msg))
                exit = true;
        } else if (movie->format_ctx && !movie->eof_reached && movie_async_dat_available(ctx)) {
            pthread_mutex_unlock(&movie->mutex);

            exit = movie_async_proc_dat(ctx);
        } else if (exit) {
            pthread_mutex_unlock(&movie->mutex);
            movie->state = AVMOVIE_ASYNC_STATE_NOP;
            movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_CLOSED, 0, NULL);
            movie->loop_count = 0;
            break;
        } else {
            pthread_cond_wait(&movie->cond, &movie->mutex);
            pthread_mutex_unlock(&movie->mutex);
        }
    }

    return NULL;
}

static int movie_async_proc_open(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    struct sched_param param;
    pthread_attr_t attr;
    pthread_t thread;
    int ret;

    ret = movie_async_send_cmd(ctx, AVMOVIE_ASYNC_OPEN, NULL, 0);

    if (movie->state != AVMOVIE_ASYNC_STATE_NOP)
        return ret;

    movie->state = AVMOVIE_ASYNC_STATE_STOPPED;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, movie->stack_size);
#ifdef FILTER_MOVIE_PRIORITY
    movie->priority = FILTER_MOVIE_PRIORITY;
#endif
    param.sched_priority = movie->priority;
    pthread_attr_setschedparam(&attr, &param);
    ret = pthread_create(&thread, &attr, movie_async_thread, ctx);
    if (ret != 0) {
        movie->state = AVMOVIE_ASYNC_STATE_NOP;
        return AVERROR(ret);
    }

    pthread_setname_np(thread, ctx->name);
    pthread_detach(thread);

    return 0;
}

static int movie_async_output_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    MovieAsyncContext *movie = ctx->priv;
    unsigned out_id = FF_OUTLINK_IDX(outlink);
    AVCodecParameters *p = NULL;

    outlink->time_base = movie->streams[out_id].time_base;
    if (!outlink->time_base.num || !outlink->time_base.den)
        outlink->time_base = AV_TIME_BASE_Q;

    switch (outlink->type) {
        case AVMEDIA_TYPE_VIDEO:
            if (movie_async_peek_info(ctx, out_id, &p)) {
                outlink->w = p->width;
                outlink->h = p->height;
                outlink->frame_rate = movie->streams[out_id].frame_rate;
            }
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (movie->streams[out_id].index < 0)
                ff_outlink_set_status(outlink, AVERROR_EOF, AV_NOPTS_VALUE);

            break;
    }

    return 0;
}

static av_cold void movie_async_uninit(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int i;

    av_assert0(movie->state == AVMOVIE_ASYNC_STATE_NOP);

    for (i = 0; i < ctx->nb_outputs; i++) {
        ff_framequeue_free(&movie->streams[i].dat_queue);
        av_freep(&ctx->output_pads[i].name);
    }

    if (movie->global_opts)
        av_dict_free(&movie->global_opts);

    av_freep(&movie->streams);
    pthread_mutex_destroy(&movie->mutex);
    pthread_cond_destroy(&movie->cond);
}

static av_cold int movie_async_init_dict(AVFilterContext *ctx, AVDictionary **options)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret = AVERROR(ENOMEM);
    int i, outputs = 2;

    AVFilterPad pad = { 0 };
    enum AVMediaType types[] = {
        AVMEDIA_TYPE_AUDIO,
        AVMEDIA_TYPE_VIDEO,
    };

    if (ctx->filter->name[0] == 'a') {
        outputs = 1;
        types[0] = AVMEDIA_TYPE_AUDIO;
    } else if (ctx->filter->name[0] == 'v') {
        outputs = 1;
        types[0] = AVMEDIA_TYPE_VIDEO;
    }

    movie->streams = av_calloc(outputs, sizeof(MovieStream));
    if (!movie->streams)
        return AVERROR(ENOMEM);

    SIMPLEQ_INIT(&movie->cmd_queue);
    SIMPLEQ_INIT(&movie->evt_queue);
    pthread_mutex_init(&movie->mutex, NULL);
    pthread_cond_init(&movie->cond, NULL);

    for (i = 0; i < outputs; i++) {
        movie->streams[i].type  = types[i];
        movie->streams[i].index = -1;

        ff_framequeue_init(&movie->streams[i].dat_queue, NULL);

        pad.type         = types[i];
        pad.config_props = movie_async_output_props;
        pad.name         = av_asprintf("output%d", i);
        if (!pad.name)
            goto error;

        if ((ret = ff_append_outpad(ctx, &pad)) < 0) {
            av_freep(&pad.name);
            goto error;
        }
    }

    if (options && *options) {
        av_dict_copy(&movie->global_opts, *options, 0);
        av_dict_free(options);
    }

    if (movie->dat_cnt > movie->dat_max)
        movie->dat_cnt = movie->dat_max;

    return 0;

error:
    movie_async_uninit(ctx);
    return ret;
}

static int movie_async_query_formats(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    AVChannelLayout list64[] = { { 0 }, { 0 } };
    AVCodecParameters *p = NULL;
    int list[] = { 0, -1 };
    AVFilterLink *outlink;
    bool ready = true;
    int i, ret;

    /* to avoid reconfiging successfully in advanced before started,
     * because once reconfiging successfully, src filter need sending frame to outlink asap,
     * or amix pending mixing when amix has other active inputs */
    if (movie->state != AVMOVIE_ASYNC_STATE_STARTED || !movie->need_reconfig)
        return FFERROR_NOT_READY;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie->streams[i].index < 0)
            continue;

        if (!movie_async_peek_info(ctx, i, &p)) {
            ready = false;
            continue;
        }

        outlink = ctx->outputs[i];

        switch (outlink->type) {
            case AVMEDIA_TYPE_AUDIO:
                list[0] = p->sample_rate;
                ff_formats_unref(&outlink->incfg.samplerates);
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->incfg.samplerates)) < 0)
                    return ret;

                if ((ret = av_channel_layout_copy(&list64[0], &p->ch_layout) < 0))
                    return ret;

                ff_channel_layouts_unref(&outlink->incfg.channel_layouts);
                if ((ret = ff_channel_layouts_ref(ff_make_channel_layout_list(list64),
                                                  &outlink->incfg.channel_layouts)) < 0)
                    return ret;

            default:
                list[0] = p->codec_id;
                if (avcodec_is_pcm_lossless(list[0]))
                    list[0] = AV_CODEC_ID_RAWAUDIO;

                /* codec id */
                ff_formats_unref(&outlink->incfg.codecs);
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->incfg.codecs)) < 0)
                    return ret;

                list[0] = p->format;

                /* format */
                ff_formats_unref(&outlink->incfg.formats);
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->incfg.formats)) < 0)
                    return ret;

                break;
        }
    }

    return ready ? 0 : FFERROR_NOT_READY;
}

static int movie_async_reconfig(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int i, ret = 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie->streams[i].index < 0)
            continue;
        if (ff_outlink_get_status(ctx->outputs[i]) == 0)
            return 0;
        if (movie_async_dat_count(ctx, i) < movie->dat_cnt)
            return 0;
    }

    ret = avfilter_graph_reconfig(ctx->graph, ctx);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "reconfig failed:%s \n", ctx->name);
        return ret;
    }
    movie->need_reconfig = false;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (!ff_outlink_get_status(ctx->outputs[i]))
            ctx->outputs[i]->frame_wanted_out = 1;
    }

    return 0;
}

static int movie_async_activate(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int status, i, ret = AVERROR(EAGAIN);
    AVFilterLink *link;
    AVFrame *frame;

    movie_async_proc_event(ctx);

    ret = movie_async_reconfig(ctx);
    if (ret < 0)
        return ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie->streams[i].index < 0)
            continue;

        if (!movie_async_dat_count(ctx, i))
            continue;

        link = ctx->outputs[i];
        status = ff_outlink_get_status(link);

        if (status < 0 || !link->incfg.formats)
            continue;

        if (!ff_outlink_frame_wanted(link))
            continue;

        frame = movie_async_recv_dat(ctx, i);
        if (!frame)
            continue;

        ret = ff_filter_frame(link, frame);
        if (ret < 0) {
            movie->eof_reached = true;
            movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_COMPLETED, ret, NULL);
        } else if (!movie->frame_sent)
            movie->frame_sent = true;
    }

    return ret;
}

static int movie_async_compute_src_latency(AVFilterContext *ctx, int64_t *latency)
{
    MovieAsyncContext *movie = ctx->priv;
    int64_t src_latency = 0;
    int i, nb_frames, pad;
    AVFrame *frame;
    AVPacket *pkt;

    for (pad = 0; pad < ctx->nb_outputs; pad++) {
        if (movie->streams[pad].type == AVMEDIA_TYPE_AUDIO)
            break;
    }

    if (pad == ctx->nb_outputs)
        return AVERROR(EINVAL);

    pthread_mutex_lock(&movie->mutex);
    nb_frames = ff_framequeue_queued_frames(&movie->streams[pad].dat_queue);
    for (i = 0; i < nb_frames; i++) {
        frame = ff_framequeue_peek(&movie->streams[pad].dat_queue, i);
        unwrap_frame(frame, &pkt, NULL);
        src_latency += av_rescale_q(pkt->duration, movie->streams[pad].time_base, AV_TIME_BASE_Q);
    }
    pthread_mutex_unlock(&movie->mutex);

    *latency = src_latency;
    return pad;
}

static int movie_async_get_position(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;
    int64_t latency;

    if (!res || !res_len)
        return AVERROR(EINVAL);

    if (movie_async_compute_src_latency(ctx, &latency) >= 0)
        latency = av_rescale_q(latency, AV_TIME_BASE_Q, av_make_q(1, 1000));

    snprintf(res, res_len, "%u", movie->current_ms - (unsigned)latency);
    return 0;
}

static int movie_async_get_duration(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;

    if (!res || !res_len)
        return AVERROR(EINVAL);

    snprintf(res, res_len, "%u", movie->duration_ms);
    return 0;
}

static int movie_async_get_latency(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;
    int64_t timestamp = 0, sink_latency = 0, src_latency = 0;
    int64_t *data[2] = {&timestamp, &sink_latency};
    AVFilterContext* sink_filter;
    int ret, pad;

    if (!res || !res_len)
        return AVERROR(EINVAL);

    pad = movie_async_compute_src_latency(ctx, &src_latency);
    if (pad < 0)
        return pad;

    if (ff_outlink_get_status(ctx->outputs[pad]))
        return AVERROR(EINVAL);

    /* find the sink */
    sink_filter = avfilter_find_on_link(ctx, "adevsink", NULL, true, NULL);
    if (!sink_filter)
        return AVERROR(EINVAL);

    ret = avfilter_process_command(sink_filter, "get_timestamp", NULL, (char *)&data, sizeof(data), 0);
    if (ret < 0)
        return ret;

    /* rescale adevsink frames as decoding sample_rate */
    sink_latency = av_rescale_q(sink_latency, AV_TIME_BASE_Q, av_make_q(1, ctx->outputs[pad]->sample_rate));

    /* rescale src filter frames as decoding sample_rate */
    src_latency = av_rescale_q(src_latency, AV_TIME_BASE_Q, av_make_q(1, ctx->outputs[pad]->sample_rate));

    snprintf(res, res_len, "%"PRId64, sink_latency > src_latency ? sink_latency : src_latency);
    return 0;
}

static int movie_async_dump(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;
    AVCodecParameters *param;
    int pos = 0, ret, i, idx;

    ret = snprintf(res, res_len, "st: %d", movie->state);
    pos += ret;

    if (!movie->streams)
        return 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        idx = movie->streams[i].index;
        if (idx < 0 || !movie->streams[idx].codecpar)
            continue;

        param = movie->streams[idx].codecpar;
        if (movie->streams[i].type == AVMEDIA_TYPE_AUDIO) {
            ret = snprintf(res + pos, res_len - pos, ", A: %d %s %"PRIu64" %d %d %zu",
                                    movie->streams[i].index,
                                    avcodec_get_name(param->codec_id),
                                    movie->streams[idx].codecpar->bit_rate,
                                    param->sample_rate,
                                    param->ch_layout.nb_channels,
                                    ff_framequeue_queued_frames(&movie->streams[i].dat_queue));
        } else {
            ret = snprintf(res + pos, res_len - pos, ", V: %d %s %d %d %zu",
                                    movie->streams[i].index,
                                    avcodec_get_name(param->codec_id),
                                    param->width,
                                    param->height,
                                    ff_framequeue_queued_frames(&movie->streams[i].dat_queue));
        }

        if (ret < 0)
            return ret;

        pos += ret;
        if (pos >= res_len)
            break;
    }

    return 0;
}

static int movie_async_process_proc_cmd(AVFilterContext *ctx, const char *cmd, const char *args)
{
    char *ptr;
    int ret;

    if (!cmd || !args)
        return AVERROR(EINVAL);

    ptr = av_asprintf("%s=%s", cmd, args);
    if (!ptr)
        return AVERROR(ENOMEM);

    ret = movie_async_send_cmd(ctx, AVMOVIE_ASYNC_PROCESS_COMMAND, ptr, strlen(ptr) + 1);
    av_freep(&ptr);

    return ret;
}

static int movie_async_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                       char *res, int res_len, int flags)
{
    MovieAsyncContext *movie = ctx->priv;
    AVMovieAsyncEventCookie *event = NULL;

    if (!strcmp(cmd, "open")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s open.\n", __func__, ctx->name);
        movie->need_reconfig = false;
        return movie_async_proc_open(ctx);
    } else if (!strcmp(cmd, "set_event")) {
        event = (AVMovieAsyncEventCookie *)args;

        movie->event  = event->event;
        movie->cookie = event->cookie;

        return 0;
    } else if (!strcmp(cmd, "set_options")) {
        av_dict_parse_string(&movie->global_opts, args, "=", ":", 0);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_SET_OPTIONS, args, strlen(args) + 1);
    } else if (!strcmp(cmd, "set_loop")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_SET_LOOP, args, strlen(args) + 1);
    }  else if (!strcmp(cmd, "prepare")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s prepare %s.\n", __func__, ctx->name, args);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_PREPARE, args, strlen(args) + 1);
    }  else if (!strcmp(cmd, "start")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s start.\n", __func__, ctx->name);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_START, NULL, 0);
    } else if (!strcmp(cmd, "pause")) {

        av_log(ctx, AV_LOG_INFO, "%s filter %s pause.\n", __func__, ctx->name);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_PAUSE, NULL, 0);
    } else if (!strcmp(cmd, "seek")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_SEEK, args, strlen(args) + 1);
    } else if (!strcmp(cmd, "stop")) {
        av_log(ctx, AV_LOG_INFO, "%s filter %s stop.\n", __func__, ctx->name);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_STOP, NULL, 0);
    } else if (!strcmp(cmd, "reset")) {
        movie_async_clear_queue(ctx, AVMOVIE_ASYNC_CMD_QUEUE_IDX);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_RESET, NULL, 0);
    } else if (!strcmp(cmd, "close")) {
        movie_async_clear_queue(ctx, AVMOVIE_ASYNC_CMD_QUEUE_IDX);
        av_log(ctx, AV_LOG_INFO, "%s filter %s close.\n", __func__, ctx->name);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_CLOSE, args, strlen(args) + 1);
    } else if (!strcmp(cmd, "get_state")) {
        av_log(ctx, AV_LOG_INFO, "%s get_state %d.\n", ctx->name, movie->state);
        if (!res || !res_len)
            return AVERROR(EINVAL);

        snprintf(res, res_len, "%d", movie->state);
        return 0;
    } else if (!strcmp(cmd, "get_playing")) {
        if (!res || !res_len)
            return AVERROR(EINVAL);

        snprintf(res, res_len, "%d", movie->state == AVMOVIE_ASYNC_STATE_STARTED);
        return 0;
    } else if (!strcmp(cmd, "get_position")) {
        memset(res, 0, res_len);
        for (int i = 0; i < ctx->nb_outputs; i++) {
            int ret = avfilter_forward_command(ctx, i, NULL, "get_position", NULL, res, res_len, 0);
            if (ret >= 0) {
                unsigned pos = movie->lastseek_ms + strtoul(res, NULL, 0);
                snprintf(res, res_len, "%u", FFMIN(pos, movie->duration_ms));
                return ret;
            }
        }

        return movie_async_get_position(ctx, res, res_len);
    } else if (!strcmp(cmd, "get_duration")) {
        return movie_async_get_duration(ctx, res, res_len);
    } else if (!strcmp(cmd, "get_latency")) {
        return movie_async_get_latency(ctx, res, res_len);
    } else if (!strcmp(cmd, "volume") || !strcmp(cmd, "get_volume")) {
        for (int i = 0; i < ctx->nb_outputs; i++)
            avfilter_forward_command(ctx, i, "all", cmd, args, res, res_len, AVFILTER_CMD_FLAG_ONE);

        return 0;
    } else if (!strcmp(cmd, "dump")) {
        return movie_async_dump(ctx, res, res_len);
    } else if (!res && !res_len) {
        return movie_async_process_proc_cmd(ctx, cmd, args);
    } else {
        return AVERROR(ENOSYS);
    }
}

static int movie_async_forward_command(AVFilterContext *ctx, int pad_idx, const char* target, const char *cmd,
                                       const char *arg, char *res, int res_len, int flags)
{
    MovieAsyncContext *movie = ctx->priv;
    int i;

    if (!strcmp(cmd, "completed")) {
        movie->streams[pad_idx].completed = true;
        ctx->outputs[pad_idx]->frame_wanted_out = 1;
        av_log(ctx, AV_LOG_INFO, "%s stream %d %s completed.\n",
               ctx->name, pad_idx, av_get_media_type_string(movie->streams[pad_idx].type));

        for (i = 0; i < ctx->nb_outputs; i++) {
            if (movie->streams[i].index < 0)
                continue;
            if (movie->streams[i].completed == false)
                break;
        }

        if (i == ctx->nb_outputs)
            return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_COMPLETED, NULL, 0);

        if (movie->streams[pad_idx].type == AVMEDIA_TYPE_AUDIO)
            return movie_async_send_vsyncmode(ctx, false);

        return 0;
    } else if (!strcmp(cmd, "get_options")) {
        AVDictionary **dst = (AVDictionary **)res;
        return av_dict_copy(dst, movie->global_opts, 0);
    } else {
        av_log(ctx, AV_LOG_ERROR, "src:%s unsupported command:%s.\n", ctx->name, cmd);
        return AVERROR(ENOSYS);
    }

    return 0;
}

static const struct AVClass *movie_child_class_iterate(void **iter)
{
    const AVClass *c = *iter;

    if (!c)
        c = avformat_get_class();
    else
        c = NULL;

    *iter = (void*)(uintptr_t)c;
    return *iter;
}

static void *movie_async_child_next(void *obj, void *prev)
{
    MovieAsyncContext *movie = obj;

    if (!prev)
        return movie->format_ctx;
    else
        return NULL;
}

#if CONFIG_MOVIE_ASYNC_FILTER

static const AVClass movie_async_class = {
    .class_name          = "movie_async_class",
    .item_name           = av_default_item_name,
    .option              = movie_async_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = movie_async_child_next,
    .child_class_iterate = movie_child_class_iterate,
};

const AVFilter ff_avsrc_movie_async = {
    .name            = "movie_async",
    .description     = NULL_IF_CONFIG_SMALL("Read from a movie source asynchronously."),
    .priv_size       = sizeof(MovieAsyncContext),
    .priv_class      = &movie_async_class,
    .init_dict       = movie_async_init_dict,
    .uninit          = movie_async_uninit,
    FILTER_QUERY_FUNC(movie_async_query_formats),
    .activate        = movie_async_activate,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .process_command = movie_async_process_command,
    .forward_command = movie_async_forward_command,
};
#endif  /* CONFIG_MOVIE_ASYNC_FILTER */

#if CONFIG_AMOVIE_ASYNC_FILTER

static const AVClass amovie_async_class = {
    .class_name          = "amovie_async_class",
    .item_name           = av_default_item_name,
    .option              = movie_async_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = movie_async_child_next,
    .child_class_iterate = movie_child_class_iterate,
};

const AVFilter ff_avsrc_amovie_async = {
    .name            = "amovie_async",
    .description     = NULL_IF_CONFIG_SMALL("Read audio from a movie source asynchronously."),
    .priv_size       = sizeof(MovieAsyncContext),
    .init_dict       = movie_async_init_dict,
    .uninit          = movie_async_uninit,
    FILTER_QUERY_FUNC(movie_async_query_formats),
    .activate        = movie_async_activate,
    .priv_class      = &amovie_async_class,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .process_command = movie_async_process_command,
    .forward_command = movie_async_forward_command,
};

#endif /* CONFIG_AMOVIE_ASYNC_FILTER */

#if CONFIG_VMOVIE_ASYNC_FILTER

static const AVClass vmovie_async_class = {
    .class_name          = "vmovie_async_class",
    .item_name           = av_default_item_name,
    .option              = movie_async_options,
    .version             = LIBAVUTIL_VERSION_INT,
    .category            = AV_CLASS_CATEGORY_FILTER,
    .child_next          = movie_async_child_next,
    .child_class_iterate = movie_child_class_iterate,
};

const AVFilter ff_avsrc_vmovie_async = {
    .name            = "vmovie_async",
    .description     = NULL_IF_CONFIG_SMALL("Read video from a movie source asynchronously."),
    .priv_size       = sizeof(MovieAsyncContext),
    .init_dict       = movie_async_init_dict,
    .uninit          = movie_async_uninit,
    FILTER_QUERY_FUNC(movie_async_query_formats),
    .activate        = movie_async_activate,
    .priv_class      = &vmovie_async_class,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .process_command = movie_async_process_command,
    .forward_command = movie_async_forward_command,
};

#endif /* CONFIG_VMOVIE_ASYNC_FILTER */
