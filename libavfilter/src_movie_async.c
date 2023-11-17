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
    enum AVMediaType type;
    int              index;                     /**< AVStream index of AVFormatContext */
    FFFrameQueue     dat_queue;
    AVRational       time_base;
    AVRational       frame_rate;
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

    unsigned                  current_ms;     /** < current timestamp of the decoded frame */
    unsigned                  duration_ms;    /** < duration of whole stream */
    int                       loop_count;
    int                       pending_stop;

    void                      *cookie;
    av_movie_async_event_func event;
} MovieAsyncContext;

#define OFFSET(x) offsetof(MovieAsyncContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption movie_async_options[]= {
    { "datqmax",      "maximum number of dat queue",        OFFSET(dat_max),      AV_OPT_TYPE_INT,    {.i64 = 4 },      1, INT_MAX, FLAGS },
    { "datqcnt",      "prebuff frame count before playing", OFFSET(dat_cnt),      AV_OPT_TYPE_INT,    {.i64 = INT_MAX },0, INT_MAX, FLAGS },
    { "cmdqmax",      "maximum number of cmd queue",        OFFSET(cmd_max),      AV_OPT_TYPE_INT,    {.i64 = 16 },     8, 32,      FLAGS },
    { "stack_size",   "stack size of work thread",          OFFSET(stack_size),   AV_OPT_TYPE_INT,    {.i64 = 61440 },  0, INT_MAX, FLAGS },
    { "priority",     "priority of work thread",            OFFSET(priority),     AV_OPT_TYPE_INT,    {.i64 = 244 },    0, INT_MAX, FLAGS },
    { "protocol_map", "mapping of protocol",                OFFSET(protocol_map), AV_OPT_TYPE_STRING, {.str = NULL},    0, 0,       FLAGS },
    { NULL },
};

static inline bool movie_async_output_inactive(MovieAsyncContext *movie, int pad_id)
{
    return movie->streams[pad_id].index == -1;
}

static inline void movie_async_notify_event(MovieAsyncContext *movie, int event, int ret, const char *extra)
{
    if (movie->event != NULL && movie->cookie != NULL)
        movie->event(movie->cookie, event, ret, extra);
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
    AVCodecParameters *src = NULL;
    AVFrame *frame = NULL;

    pthread_mutex_lock(&movie->mutex);
    if (ff_framequeue_queued_frames(&movie->streams[pad_id].dat_queue))
        frame = ff_framequeue_peek(&movie->streams[pad_id].dat_queue, 0);
    pthread_mutex_unlock(&movie->mutex);

    if (frame && frame->opaque_ref) {
        *dst = (AVCodecParameters *)frame->opaque_ref->data;
        return true;
    }

    return false;
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

    /* As long as one data queue less than movie->dat_max, continue read */
    for (i = 0; i < ctx->nb_outputs; i++) {
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

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie_async_output_inactive(movie, i))
            continue;

        stream = &movie->streams[i];
        if (stream)
            stream->index = -1;
    }

    if (movie->format_ctx)
        avformat_close_input(&movie->format_ctx);

    if (movie->format_opt)
        av_dict_free(&movie->format_opt);

    movie->current_ms = 0;
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

    movie->format_ctx->interrupt_callback.callback = movie_async_interrupt;
    movie->format_ctx->interrupt_callback.opaque = ctx;

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
            av_log(ctx, AV_LOG_WARNING, "Failed to find best stream ret %d %s.\n", ret, av_err2str(ret));
            goto out;
        }

        stream = movie->format_ctx->streams[ret];

        /* Use specify ch_layout if possible, follow guess_input_channel_layout() in ffmpeg.c */
        if (movie->streams[i].type == AVMEDIA_TYPE_AUDIO &&
            stream->codecpar->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&stream->codecpar->ch_layout,
                                      stream->codecpar->ch_layout.nb_channels);

        movie->format_ctx->streams[i]->discard = AVDISCARD_DEFAULT;
        movie->streams[i].index      = stream->index;
        movie->streams[i].time_base  = stream->time_base;
        movie->streams[i].frame_rate = stream->r_frame_rate;
    }
    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s open decode DONE.\n", name);

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
    if (ff_outlink_get_status(ctx->outputs[pad_id]) != 0) {
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
        if (pkt->stream_index == movie->streams[i].index) {
            movie->current_ms = pkt->pts * av_q2d(movie->streams[i].time_base) * 1000;
            ret = movie_async_send_frame(ctx, pkt, i);
            if (ret < 0)
                goto out;

            /* assign to NULL when the type of AVPacket matched with outputs pad.
             * otherwise free AVPacket */
            pkt = NULL;
            break;
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
        ret = movie_async_seek(ctx, 0, false);
        movie->loop_count -= movie->loop_count > 0;
    }

    return ret;
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

        switch (event->event) {
            case AVMOVIE_ASYNC_EVENT_STARTED:
                for (i = 0; i < ctx->nb_outputs; i++)
                    avfilter_forward_command(ctx, i, NULL, "play", NULL, NULL, 0, 0);
                break;

            case AVMOVIE_ASYNC_EVENT_PAUSED:
                for (i = 0; i < ctx->nb_outputs; i++)
                    avfilter_forward_command(ctx, i, NULL, "pause", NULL, NULL, 0, 0);
                break;

            case AVMOVIE_ASYNC_EVENT_STOPPED:
                for (i = 0; i < ctx->nb_outputs; i++) {
                    avfilter_forward_command(ctx, i, NULL, "flush", NULL, NULL, 0, 0);
                    ff_avfilter_link_set_in_status(ctx->outputs[i], AVERROR_EOF, AV_NOPTS_VALUE);
                }
                break;

            case AVMOVIE_ASYNC_EVENT_CLOSED:
                movie->event  = NULL;
                movie->cookie = NULL;
                break;
        }

        movie_async_notify_event(movie, event->event, event->ret, event->extra);
        av_freep(&event);
    }
}

static bool movie_async_proc_dat(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret, i;

    ret = movie_async_read_frame(ctx);
    if (ret == AVERROR_EOF)
        ret = movie_async_loop(ctx);

    if (ret >= 0 || ret == AVERROR_EXIT)
        return false;
    else if (ret == AVERROR_EOF)
        ret = 0;

    movie->state = AVMOVIE_ASYNC_STATE_COMPLETED;
    movie_async_send_event(ctx, AVMOVIE_ASYNC_EVENT_COMPLETED, ret, NULL);

    if (!movie->pending_stop)
        return false;

    movie_async_stop(ctx);
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

            exit = movie_async_proc_cmd(ctx, msg);
        } else if (movie->format_ctx && movie_async_dat_available(ctx)) {
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
    if (movie->state != AVMOVIE_ASYNC_STATE_STARTED)
        return FFERROR_NOT_READY;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (!movie_async_peek_info(ctx, i, &p)) {
            ready = false;
            continue;
        }

        outlink = ctx->outputs[i];

        switch (outlink->type) {
            case AVMEDIA_TYPE_AUDIO:
                list[0] = p->sample_rate;
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->incfg.samplerates)) < 0)
                    return ret;

                if ((ret = av_channel_layout_copy(&list64[0], &p->ch_layout) < 0))
                    return ret;

                if ((ret = ff_channel_layouts_ref(ff_make_channel_layout_list(list64),
                                                  &outlink->incfg.channel_layouts)) < 0)
                    return ret;

            default:
                list[0] = p->codec_id;
                if (avcodec_is_pcm_lossless(list[0]))
                    list[0] = AV_CODEC_ID_RAWAUDIO;

                /* codec id */
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->incfg.codecs)) < 0)
                    return ret;

                list[0] = p->format;

                /* format */
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
    bool need_reconfig = false;
    int i, ret = 0;

    for (i = 0; i < ctx->nb_outputs; i++) {
        /* only before starting case need reconfig: link status is not 0 and data queues have frame.
         * to avoid reconfig after stopping case: link status is is not 0 and data queues are empty.*/
        if (ff_outlink_get_status(ctx->outputs[i]) != 0 &&
            movie_async_dat_count(ctx, i) >= movie->dat_cnt) {
            need_reconfig = true;
            break;
        }
    }

    if (need_reconfig) {
        ret = avfilter_graph_reconfig(ctx->graph, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "reconfig failed:%s \n", ctx->name);
            return ret;
        }

        for (i = 0; i < ctx->nb_outputs; i++) {
            if (!ff_outlink_get_status(ctx->outputs[i]))
                ctx->outputs[i]->frame_wanted_out = 1;
        }
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
        if (!movie_async_dat_count(ctx, i))
            continue;

        link = ctx->outputs[i];
        status = ff_outlink_get_status(link);

        if (status < 0 || !link->incfg.formats)
            continue;

        if (!ff_outlink_frame_wanted(link))
            continue;

        frame = movie_async_recv_dat(ctx, i);

        ret = ff_filter_frame(link, frame);
    }

    return ret;
}

static int movie_async_get_position(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;

    if (!res || !res_len)
        return AVERROR(EINVAL);

    snprintf(res, res_len, "%u", movie->current_ms);
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

static int movie_async_dump(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;
    AVCodecParameters *param;
    int pos = 0, ret, i, idx;

    ret = snprintf(res, res_len, "st: %d", movie->state);
    pos += ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie_async_output_inactive(movie, i))
            continue;

        idx = movie->streams[i].index;
        param = movie->format_ctx->streams[idx]->codecpar;

        if (movie->streams[i].type == AVMEDIA_TYPE_AUDIO) {
            ret = snprintf(res + pos, res_len - pos, ", A: %d %s %"PRIu64" %d %d %zu",
                                    movie->streams[i].index,
                                    avcodec_get_name(param->codec_id),
                                    movie->format_ctx->bit_rate,
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
        return movie_async_proc_open(ctx);
    } else if (!strcmp(cmd, "set_event")) {
        event = (AVMovieAsyncEventCookie *)args;

        movie->event  = event->event;
        movie->cookie = event->cookie;

        return 0;
    } else if (!strcmp(cmd, "set_options")) {
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
        return movie_async_get_position(ctx, res, res_len);
    } else if (!strcmp(cmd, "get_duration")) {
        return movie_async_get_duration(ctx, res, res_len);
    } else if (!strcmp(cmd, "dump")) {
        return movie_async_dump(ctx, res, res_len);
    } else if (!res && !res_len) {
        return movie_async_process_proc_cmd(ctx, cmd, args);
    } else {
        return AVERROR(ENOSYS);
    }
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
    .process_command = movie_async_process_command
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
};

#endif /* CONFIG_VMOVIE_ASYNC_FILTER */
