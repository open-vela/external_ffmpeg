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

#include <float.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

#include "libavutil/avstring.h"
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

#include "movie_async.h"
#include "filters.h"

#define AVMOVIE_ASYNC_CMD_QUEUE_IDX           (1 << 0)
#define AVMOVIE_ASYNC_DATA_QUEUE_IDX          (1 << 1)

typedef struct MovieCmd {
    struct dq_entry_s  dq_entry;
    int                cmd;
    char               data[0];
} MovieCmd;

typedef struct MovieStream {
    enum AVMediaType type;
    int              index;
    AVCodecContext   *codec_ctx;
    FFFrameQueue     dat_queue;
    int              reconfig;                  /**< whether need to do reconfig */
    AVRational       time_base;
} MovieStream;

typedef struct MovieAsyncContext {
    /* common A/V fields */
    const                     AVClass *class;

    int                       dat_max;
    int                       cmd_max;
    int                       silent_samples;
    int                       stack_size;
    int                       priority;

    MovieStream               *streams;       /**< array of all streams, one per output */
    AVFormatContext           *format_ctx;
    AVDictionary              *format_opt;
    AVDictionary              *global_opts;

    pthread_mutex_t           mutex;
    pthread_cond_t            cond;
    dq_queue_t                cmd_queue;

    int                       state;

    unsigned                  current_ms;     /** < current timestamp of the decoded frame */
    int                       loop_count;
    int                       pending_stop;

    void                      *cookie;
    av_movie_async_event_func event;
} MovieAsyncContext;

#define OFFSET(x) offsetof(MovieAsyncContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_VIDEO_PARAM

static const AVOption movie_async_options[]= {
    { "datqmax",        "maximum number of dat queue", OFFSET(dat_max),        AV_OPT_TYPE_INT, {.i64 = 4 },     2, 8,         FLAGS },
    { "cmdqmax",        "maximum number of cmd queue", OFFSET(cmd_max),        AV_OPT_TYPE_INT, {.i64 = 16 },    8, 32,        FLAGS },
    { "silent_samples", "samples of silent frame",     OFFSET(silent_samples), AV_OPT_TYPE_INT, {.i64 = 1024 },  0, 2048,      FLAGS },
    { "stack_size",     "stack size of work thread",   OFFSET(stack_size),     AV_OPT_TYPE_INT, {.i64 = 61440 }, 0, INT32_MAX, FLAGS },
    { "priority",       "priority of work thread",     OFFSET(priority),       AV_OPT_TYPE_INT, {.i64 = 244 },   0, INT16_MAX, FLAGS },
    { NULL },
};

static inline bool movie_async_output_inactive(MovieAsyncContext *movie, int pad_id)
{
    return movie->streams[pad_id].index == -1 ||
           movie->streams[pad_id].codec_ctx == NULL;
}

static inline void movie_async_notify_event(MovieAsyncContext *movie, int event, int ret, const char *extra)
{
    if (movie->event != NULL && movie->cookie != NULL)
        movie->event(movie->cookie, event, ret, extra);
}

static int movie_async_send_cmd(AVFilterContext *ctx, const int cmd, const void *data, size_t size)
{
    MovieAsyncContext *movie = ctx->priv;
    MovieCmd *msg;

    msg = av_malloc(sizeof(MovieCmd) + size);
    if (!msg)
        return AVERROR(ENOMEM);

    msg->cmd = cmd;
    memcpy(msg->data, data, size);

    pthread_mutex_lock(&movie->mutex);
    if (dq_count(&movie->cmd_queue) >= movie->cmd_max &&
        msg->cmd < AVMOVIE_ASYNC_STOP) {

        pthread_mutex_unlock(&movie->mutex);
        av_freep(&msg);

        return AVERROR(ENOMEM);
    }

    dq_addlast(&msg->dq_entry, &movie->cmd_queue);
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

static AVFrame *movie_async_alloc_empty_frame(AVFilterContext *ctx, int pad_id)
{
    MovieAsyncContext *movie = ctx->priv;
    AVFrame *out;

    out = av_frame_alloc();
    if (!out)
        return NULL;

    if (movie->streams[pad_id].codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        out->format         = movie->streams[pad_id].codec_ctx->sample_fmt;
        out->sample_rate    = movie->streams[pad_id].codec_ctx->sample_rate;
        out->channels       = movie->streams[pad_id].codec_ctx->channels;
        out->channel_layout = movie->streams[pad_id].codec_ctx->channel_layout;
    } else {
        out->format = movie->streams[pad_id].codec_ctx->pix_fmt;
        out->width  = movie->streams[pad_id].codec_ctx->width;
        out->height = movie->streams[pad_id].codec_ctx->height;
    }

    return out;
}

static bool movie_async_peek_info(AVFilterContext *ctx, int pad_id, AVCodecParameters *param)
{
    MovieAsyncContext *movie = ctx->priv;
    AVFrame *src = NULL;
    bool audio = true;

    if (ctx->outputs[pad_id]->type == AVMEDIA_TYPE_VIDEO)
        audio = false;

    pthread_mutex_lock(&movie->mutex);
    if (ff_framequeue_queued_frames(&movie->streams[pad_id].dat_queue))
        src = ff_framequeue_peek(&movie->streams[pad_id].dat_queue, 0);

    if (!src) {
        pthread_mutex_unlock(&movie->mutex);
        return false;
    }

    param->format = src->format;
    if (audio) {
        param->sample_rate    = src->sample_rate;
        param->channel_layout = src->channel_layout;
    } else {
        param->width  = src->width;
        param->height = src->height;
    }

    pthread_mutex_unlock(&movie->mutex);
    return true;
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

static bool movie_async_dat_empty(AVFilterContext *ctx, int pad_id)
{
    MovieAsyncContext *movie = ctx->priv;
    bool empty;

    pthread_mutex_lock(&movie->mutex);
    empty = ff_framequeue_queued_frames(&movie->streams[pad_id].dat_queue) == 0;
    pthread_mutex_unlock(&movie->mutex);

    return empty;
}

static int movie_async_interrupt(void *opaque)
{
    AVFilterContext *ctx = opaque;
    MovieAsyncContext *movie = ctx->priv;
    dq_entry_t *entry;
    MovieCmd *msg;
    int interrupt = 0;

    pthread_mutex_lock(&movie->mutex);
    for (entry = dq_peek(&movie->cmd_queue); entry; entry = dq_next(entry)) {

        msg = (MovieCmd *)entry;
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
        while ((msg = (MovieCmd *)dq_remfirst(&movie->cmd_queue)) != NULL) {
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

static bool movie_async_dat_allfree(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (ff_framequeue_queued_frames(&movie->streams[i].dat_queue) >= movie->dat_max)
            return false;
    }

    return true;
}

static int movie_async_open_decoder(AVFilterContext *ctx, MovieStream *stream, AVCodecParameters *codecpar)
{
    AVCodec *codec;
    int ret;

    codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        av_log(ctx, AV_LOG_ERROR, "Failed to find any codec\n");
        return AVERROR(EINVAL);
    }

    stream->codec_ctx = avcodec_alloc_context3(codec);
    if (!stream->codec_ctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(stream->codec_ctx, codecpar);
    if (ret < 0)
        return ret;

    stream->codec_ctx->refcounted_frames = 1;
    stream->codec_ctx->thread_count = ff_filter_get_nb_threads(ctx);

    if ((ret = avcodec_open2(stream->codec_ctx, codec, NULL)) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to open codec ret %d %s.\n", ret, av_err2str(ret));
        return ret;
    }

    return 0;
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
        if (stream) {
            stream->index = -1;
            avcodec_close(stream->codec_ctx);
            avcodec_free_context(&stream->codec_ctx);
        }
    }

    if (movie->format_ctx)
        avformat_close_input(&movie->format_ctx);

    if (movie->format_opt)
        av_dict_free(&movie->format_opt);

    movie->current_ms = 0;
}

static AVFrame *movie_async_alloc_silent_frame(AVFilterContext *ctx, int pad_id)
{
    MovieAsyncContext *movie = ctx->priv;
    AVFrame *out;
    int ret;

    out = movie_async_alloc_empty_frame(ctx, pad_id);
    if (!out)
        return NULL;

    out->nb_samples = movie->silent_samples;
    ret = av_frame_get_buffer(out, 0);
    if (ret < 0) {
        av_frame_free(&out);
        return NULL;
    }

    av_samples_set_silence(out->extended_data, 0, out->nb_samples, out->channels, out->format);
    return out;
}

static int movie_async_send_silent_frame(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    AVFrame *out;
    int ret, i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie_async_output_inactive(movie, i))
            continue;

        if (ctx->outputs[i]->type != AVMEDIA_TYPE_AUDIO)
            continue;

        out = movie_async_alloc_silent_frame(ctx, i);
        if (!out)
            return AVERROR(ENOMEM);

        ret = movie_async_send_dat(ctx, i, out);
        if (ret < 0) {
            av_frame_free(&out);
            return ret;
        }
    }

    return 0;
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

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie_async_output_inactive(movie, i))
            continue;

        avcodec_flush_buffers(movie->streams[i].codec_ctx);
    }

    /* flush data of dat queue, decode, send silence frame to next */
    if (flush) {
        movie_async_clear_queue(ctx, AVMOVIE_ASYNC_DATA_QUEUE_IDX);
        ret = movie_async_send_silent_frame(ctx);
        if (ret < 0)
            goto end;
    }

    movie->current_ms = ms;

end:
    movie_async_notify_event(movie, AVMOVIE_ASYNC_EVENT_SEEKED, ret, NULL);
    return ret;
}

static int movie_async_open_demuxer(AVFilterContext *ctx, const char *filename)
{
    MovieAsyncContext *movie = ctx->priv;
    AVInputFormat *iformat = NULL;
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

    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s start open input.\n", filename);
    ret = avformat_open_input(&movie->format_ctx, filename, iformat, &movie->format_opt);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR,
               "Failed to avformat_open_input '%s' ret %d, %s.\n", filename, ret, av_err2str(ret));
        goto out;
    }

    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s open input done.\n", filename);

    ret = avformat_find_stream_info(movie->format_ctx, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "Failed to find stream info ret %d, %s.\n", ret, av_err2str(ret));
        goto out;
    }

    for (i = 0; i < movie->format_ctx->nb_streams; i++)
        movie->format_ctx->streams[i]->discard = AVDISCARD_ALL;

    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s find stream info done.\n", filename);

    for (i = 0; i < ctx->nb_outputs; i++) {
        ret = av_find_best_stream(movie->format_ctx, movie->streams[i].type, -1, -1, NULL, 0);
        if (ret < 0) {
            av_log(ctx, AV_LOG_WARNING, "Failed to find best stream ret %d %s.\n", ret, av_err2str(ret));
            goto out;
        }

        stream = movie->format_ctx->streams[ret];
        ret = movie_async_open_decoder(ctx, &movie->streams[i], stream->codecpar);
        if (ret < 0)
            goto out;

        movie->format_ctx->streams[i]->discard = AVDISCARD_DEFAULT;

        if (movie->streams[i].codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO &&
            !movie->streams[i].codec_ctx->channel_layout) {
            movie->streams[i].codec_ctx->channel_layout =
                av_get_default_channel_layout(stream->codecpar->channels);
        }
        movie->streams[i].index = stream->index;
        movie->streams[i].time_base = stream->time_base;
    }
    av_log(ctx, AV_LOG_INFO, "DEBUG: url %s open decode DONE.\n", filename);

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
    movie_async_notify_event(movie, AVMOVIE_ASYNC_EVENT_PREPARED, ret, NULL);
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

    movie_async_notify_event(movie, AVMOVIE_ASYNC_EVENT_STARTED, ret, NULL);
}

static void movie_async_pause(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int ret = AVERROR(EPERM);

    if (movie->state == AVMOVIE_ASYNC_STATE_STARTED) {
        ret = movie_async_send_silent_frame(ctx);
        if (ret >= 0)
            movie->state = AVMOVIE_ASYNC_STATE_PAUSED;
    }

    movie_async_notify_event(movie, AVMOVIE_ASYNC_EVENT_PAUSED, ret, NULL);
}

static void movie_async_stop(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int i, ret = 0;
    AVFrame *out;

    if (movie->state == AVMOVIE_ASYNC_STATE_STOPPED)
        return;

    if (movie->state != AVMOVIE_ASYNC_STATE_PREPARED) {
        for (i = 0; i < ctx->nb_outputs; i++) {
            if (movie_async_output_inactive(movie, i))
                continue;

            out = movie_async_alloc_empty_frame(ctx, i);
            if (!out)
                goto out;

            ret = movie_async_send_dat(ctx, i, out);
            if (ret < 0) {
                av_frame_free(&out);
                goto out;
            }
        }
    }

    movie_async_close_demuxer(ctx);

    movie->pending_stop = 0;
    movie->state        = AVMOVIE_ASYNC_STATE_STOPPED;

out:
    movie_async_notify_event(movie, AVMOVIE_ASYNC_EVENT_STOPPED, ret, NULL);
}

static int movie_async_read_frame(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    AVPacket pkt = { 0 };
    int i, ret;

    /* read a new packet from input stream */
    ret = av_read_frame(movie->format_ctx, &pkt);
    if (ret == AVERROR_EOF) {
        /* EOF -> set all decoders for flushing */
        for (i = 0; i < ctx->nb_outputs; i++) {
            if (movie_async_output_inactive(movie, i))
                continue;

            ret = avcodec_send_packet(movie->streams[i].codec_ctx, NULL);
            if (ret < 0 && ret != AVERROR_EOF)
                return ret;
        }
    }

    if (ret < 0)
        return ret;

    /* send the packet to its decoder, if any */
    for (i = 0; i < ctx->nb_outputs; i++) {

        if (!movie_async_output_inactive(movie, i) &&
            pkt.stream_index == movie->streams[i].index) {
            ret = avcodec_send_packet(movie->streams[i].codec_ctx, &pkt);
            break;
        }
    }

    av_packet_unref(&pkt);
    return ret;
}

static int movie_async_dec_frame(AVFilterContext *ctx, int pad_id, AVFrame **oframe)
{
    MovieAsyncContext *movie = ctx->priv;
    AVFrame *frame;
    int ret;

    frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    ret = avcodec_receive_frame(movie->streams[pad_id].codec_ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return ret;
    }

    movie->current_ms = frame->pts * av_q2d(movie->streams[pad_id].time_base) * 1000;

    *oframe = frame;
    return 0;
}

static int movie_async_dec_frames(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int got_frame = 0;
    AVFrame *frame;
    int ret, i;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (movie_async_output_inactive(movie, i))
            continue;

        /* read frame from decoder, add frame queue */
        ret = movie_async_dec_frame(ctx, i, &frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            continue;
        else if (ret < 0)
            return ret;

        ret = movie_async_send_dat(ctx, i, frame);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }

        got_frame = 1;
    }

    return got_frame ? 0 : ret;
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

static bool movie_async_proc_dat(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    AVFrame *frame;
    int ret;

    ret = movie_async_dec_frames(ctx);
    if (ret == AVERROR(EAGAIN)) {
        ret = movie_async_read_frame(ctx);
        if (ret == AVERROR_EOF) {
            do {
                ret = movie_async_dec_frames(ctx);
            } while (ret == 0);
        }

        if (ret == AVERROR_EOF)
            ret = movie_async_loop(ctx);
    }

    if (ret >= 0)
        return false;
    else if (ret == AVERROR_EOF)
        ret = 0;

    if (movie_async_send_silent_frame(ctx) < 0)
        ret = AVERROR(ENOMEM);

    movie->state = AVMOVIE_ASYNC_STATE_COMPLETED;
    movie_async_notify_event(movie, AVMOVIE_ASYNC_EVENT_COMPLETED, ret, NULL);

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
        case AVMOVIE_ASYNC_SET_EVENT:
            event = (AVMovieAsyncEventCookie *)msg->data;

            movie->event  = event->event;
            movie->cookie = event->cookie;
            break;

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

            av_opt_set(movie, msg->data, args, AV_OPT_SEARCH_CHILDREN);
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
        if (dq_count(&movie->cmd_queue) > 0) {
            msg = (MovieCmd *)dq_remfirst(&movie->cmd_queue);
            pthread_mutex_unlock(&movie->mutex);

            exit = movie_async_proc_cmd(ctx, msg);
        } else if (movie->state == AVMOVIE_ASYNC_STATE_STARTED && movie_async_dat_allfree(ctx)) {
            pthread_mutex_unlock(&movie->mutex);

            exit = movie_async_proc_dat(ctx);
        } else if (exit) {
            movie->state = AVMOVIE_ASYNC_STATE_NOP;
            pthread_mutex_unlock(&movie->mutex);
            movie->event  = NULL;
            movie->cookie = NULL;
            break;
        } else {
            pthread_cond_wait(&movie->cond, &movie->mutex);
            pthread_mutex_unlock(&movie->mutex);
        }
    }

    return NULL;
}

static int movie_async_open(AVFilterContext *ctx)
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

    pthread_setname_np(thread, "media_src_movie");
    pthread_detach(thread);

    return 0;
}

static int movie_async_output_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    MovieAsyncContext *movie = ctx->priv;
    unsigned out_id = FF_OUTLINK_IDX(outlink);
    AVCodecParameters p;

    outlink->time_base = movie->streams[out_id].time_base;
    if (!outlink->time_base.num || !outlink->time_base.den)
        outlink->time_base = AV_TIME_BASE_Q;

    switch (outlink->type) {
        case AVMEDIA_TYPE_VIDEO:
            if (movie_async_peek_info(ctx, out_id, &p)) {
                outlink->w = p.width;
                outlink->h = p.height;
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
    int i, outputs;

    AVFilterPad pad = { 0 };
    enum AVMediaType types[] = {
        AVMEDIA_TYPE_AUDIO,
        AVMEDIA_TYPE_VIDEO,
    };

    outputs = 1;
    if (ctx->filter->name[0] != 'a')
        outputs++;

    movie->streams = av_calloc(outputs, sizeof(MovieStream));
    if (!movie->streams)
        return AVERROR(ENOMEM);

    dq_init(&movie->cmd_queue);
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

        if ((ret = ff_insert_outpad(ctx, i, &pad)) < 0) {
            av_freep(&pad.name);
            goto error;
        }
    }

    if (options && *options) {
        av_dict_copy(&movie->global_opts, *options, 0);
        av_dict_free(options);
    }

    return 0;

error:
    movie_async_uninit(ctx);
    return ret;
}

static int movie_async_query_formats(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int64_t list64[] = { 0, -1 };
    int list[] = { 0, -1 };
    AVFilterLink *outlink;
    AVCodecParameters p;
    int flags = false;
    int i, ret;

    for (i = 0; i < ctx->nb_outputs; i++) {
        if (!movie_async_peek_info(ctx, i, &p)) {
            movie->streams[i].reconfig = 1;
            continue;
        }

        flags = true;
        outlink = ctx->outputs[i];

        switch (outlink->type) {
            case AVMEDIA_TYPE_AUDIO:
                list[0] = p.sample_rate;
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->in_samplerates)) < 0)
                    return ret;

                list64[0] = p.channel_layout;
                if ((ret = ff_channel_layouts_ref(avfilter_make_format64_list(list64),
                                                  &outlink->in_channel_layouts)) < 0)
                    return ret;

            default:
                list[0] = p.format;
                if ((ret = ff_formats_ref(ff_make_format_list(list), &outlink->in_formats)) < 0)
                    return ret;

                break;
        }

        movie->streams[i].reconfig = 0;
    }

    return flags ? 0 : FFERROR_NOT_READY;
}

static int movie_async_activate(AVFilterContext *ctx)
{
    MovieAsyncContext *movie = ctx->priv;
    int status, i, ret = AVERROR(EAGAIN);
    AVFilterLink *link;
    AVFrame *frame;
    bool active;

    for (i = 0; i < ctx->nb_outputs; i++) {
        active = false;

        if (movie_async_dat_empty(ctx, i))
            continue;

        if (movie->streams[i].reconfig) {
            avfilter_graph_reconfig(ctx->graph, NULL);
            active = true;
        }

        link = ctx->outputs[i];
        status = ff_outlink_get_status(link);

        if (status < 0 || !link->in_formats)
            continue;

        if (!active && !ff_outlink_frame_wanted(link))
            continue;

        frame = movie_async_recv_dat(ctx, i);

        if (!frame->linesize[0]) {
            ff_avfilter_link_set_in_status(link, AVERROR_EOF, AV_NOPTS_VALUE);
            movie->streams[i].reconfig = 1;
            av_frame_free(&frame);
        } else {
            ret = ff_filter_frame(link, frame);
        }
    }

    return ret;
}

static int movie_async_get_position(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;
    unsigned msec;

    if (!res || !res_len)
        return AVERROR(EINVAL);

    msec = movie->current_ms;
    snprintf(res, res_len, "%u", msec);
    return 0;
}

static int movie_async_get_duration(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;
    int64_t duration;
    unsigned msec;

    if (!res || !res_len)
        return AVERROR(EINVAL);

    if (!movie->format_ctx || movie->format_ctx->duration == AV_NOPTS_VALUE)
        return AVERROR(EPERM);

    msec = av_rescale(movie->format_ctx->duration, 1000, AV_TIME_BASE);
    snprintf(res, res_len, "%u", msec);

    return 0;
}

static int movie_async_dump(AVFilterContext *ctx, char *res, int res_len)
{
    MovieAsyncContext *movie = ctx->priv;
    int pos = 0, ret, i;

     ret = snprintf(res, res_len, "st: %d", movie->state);
     pos += ret;

     for (i = 0; i < ctx->nb_outputs; i++) {
         if (movie_async_output_inactive(movie, i))
             continue;

         if (movie->streams[i].codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
             ret = snprintf(res + pos, res_len - pos, ", A: %d %s %lld %d %d %d",
                                     movie->streams[i].index,
                                     avcodec_get_name(movie->streams[i].codec_ctx->codec_id),
                                     movie->format_ctx->bit_rate,
                                     movie->streams[i].codec_ctx->sample_rate,
                                     movie->streams[i].codec_ctx->channels,
                                     ff_framequeue_queued_frames(&movie->streams[i].dat_queue));
         } else {
             ret = snprintf(res + pos, res_len - pos, ", V: %d %s %d %d %d",
                                     movie->streams[i].index,
                                     avcodec_get_name(movie->streams[i].codec_ctx->codec_id),
                                     movie->streams[i].codec_ctx->width,
                                     movie->streams[i].codec_ctx->height,
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
    MovieAsyncContext *movie = ctx->priv;
    int len, ret;
    char *ptr;

    if (!cmd || !args)
        return AVERROR(EINVAL);

    len = strlen(cmd) + (args ? strlen(args) : 1) + 2;
    ptr = av_malloc(len);
    if (!ptr)
        return AVERROR(ENOMEM);

    snprintf(ptr, len, "%s=%s", cmd, args);
    ret = movie_async_send_cmd(ctx, AVMOVIE_ASYNC_PROCESS_COMMAND, ptr, len);
    free(ptr);

    return ret;
}

static int movie_async_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                       char *res, int res_len, int flags)
{
    MovieAsyncContext *movie = ctx->priv;

    if (!strcmp(cmd, "open")) {
        return movie_async_open(ctx);
    } else if (!strcmp(cmd, "set_event")) {
        if (!args)
            return AVERROR(EINVAL);

        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_SET_EVENT, args, sizeof(struct AVMovieAsyncEventCookie));
    } else if (!strcmp(cmd, "set_options")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_SET_OPTIONS, args, strlen(args) + 1);
    } else if (!strcmp(cmd, "set_loop")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_SET_LOOP, args, strlen(args) + 1);
    }  else if (!strcmp(cmd, "prepare")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_PREPARE, args, strlen(args) + 1);
    }  else if (!strcmp(cmd, "start")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_START, NULL, 0);
    } else if (!strcmp(cmd, "pause")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_PAUSE, NULL, 0);
    } else if (!strcmp(cmd, "seek")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_SEEK, args, strlen(args) + 1);
    } else if (!strcmp(cmd, "stop")) {
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_STOP, NULL, 0);
    } else if (!strcmp(cmd, "reset")) {
        movie_async_clear_queue(ctx, AVMOVIE_ASYNC_CMD_QUEUE_IDX);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_RESET, NULL, 0);
    } else if (!strcmp(cmd, "close")) {
        movie_async_clear_queue(ctx, AVMOVIE_ASYNC_CMD_QUEUE_IDX);
        return movie_async_send_cmd(ctx, AVMOVIE_ASYNC_CLOSE, args, strlen(args) + 1);
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

static const struct AVClass* movie_child_class_next(const struct AVClass *prev)
{
    if (!prev)
        return avformat_get_class();
    else if (prev == avformat_get_class())
        return avcodec_get_class();
    else
        return NULL;
}

static void* movie_async_child_next(void *obj, void *prev)
{
    MovieAsyncContext *movie = obj;

    if (!prev) {
        return movie->format_ctx;
    } else if (prev == movie->format_ctx) {
        return movie->streams[0].codec_ctx;
    } else {
        return NULL;
    }
}

#if CONFIG_MOVIE_ASYNC_FILTER

static const AVClass movie_async_class = {
    .class_name       = "movie_async_class",
    .item_name        = av_default_item_name,
    .option           = movie_async_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
    .child_next       = movie_async_child_next,
    .child_class_next = movie_child_class_next,
};

AVFilter ff_avsrc_movie_async = {
    .name            = "movie_async",
    .description     = NULL_IF_CONFIG_SMALL("Read from a movie source asynchronously."),
    .priv_size       = sizeof(MovieAsyncContext),
    .priv_class      = &movie_async_class,
    .init_dict       = movie_async_init_dict,
    .uninit          = movie_async_uninit,
    .query_formats   = movie_async_query_formats,
    .activate        = movie_async_activate,
    .inputs          = NULL,
    .outputs         = NULL,
    .flags           = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .process_command = movie_async_process_command
};
#endif  /* CONFIG_MOVIE_ASYNC_FILTER */

#if CONFIG_AMOVIE_ASYNC_FILTER

static const AVClass amovie_async_class = {
    .class_name       = "amovie_async_class",
    .item_name        = av_default_item_name,
    .option           = movie_async_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .category         = AV_CLASS_CATEGORY_FILTER,
    .child_next       = movie_async_child_next,
    .child_class_next = movie_child_class_next,
};

AVFilter ff_avsrc_amovie_async = {
    .name            = "amovie_async",
    .description     = NULL_IF_CONFIG_SMALL("Read audio from a movie source asynchronously."),
    .priv_size       = sizeof(MovieAsyncContext),
    .init_dict       = movie_async_init_dict,
    .uninit          = movie_async_uninit,
    .query_formats   = movie_async_query_formats,
    .activate        = movie_async_activate,
    .inputs          = NULL,
    .outputs         = NULL,
    .priv_class      = &amovie_async_class,
    .flags           = AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    .process_command = movie_async_process_command,
};

#endif /* CONFIG_AMOVIE_ASYNC_FILTER */
