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
 * Transform audio data to another core by rpmsg socket
 */

#include <netpacket/rpmsg.h>
#include <poll.h>

#include "libavcodec/avcodec.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"
#include "filters.h"
#include "rpmsg.h"

#define LISTEN_BACKLOG 1

typedef struct RpmsgSinkContext {
    const    AVClass *class;
    int      fd;
    char     *rp_name;
    AVBPrint bprint;
    bool     frame_request;
    bool     is_connected;
    size_t   remain_length;
} RpmsgSinkContext;

static av_cold int rpmsgsink_init(AVFilterContext *ctx)
{
    RpmsgSinkContext *priv = ctx->priv;
    struct sockaddr_rpmsg addr;
    int ret;

    if (priv->rp_name == NULL)
        return AVERROR(EINVAL);

    priv->fd = socket(PF_RPMSG, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (priv->fd < 0)
        return AVERROR(errno);

    addr.rp_family = AF_RPMSG;
    av_strlcpy(addr.rp_name, priv->rp_name, RPMSG_SOCKET_NAME_SIZE);

    ret = bind(priv->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    ret = listen(priv->fd, LISTEN_BACKLOG);
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    return 0;
out:
    close(priv->fd);
    return ret;
}

static av_cold void rpmsgsink_uninit(AVFilterContext *ctx)
{
    RpmsgSinkContext *priv = ctx->priv;

    av_bprint_finalize(&priv->bprint, NULL);
    priv->frame_request = false;
    priv->is_connected = false;
    priv->remain_length = 0;
    close(priv->fd);
}

static int rpmsgsink_config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    RpmsgSinkContext *priv = ctx->priv;
    RpmsgInfo info;
    ssize_t ret;

    if (priv->is_connected) {
        info.flag                 = RPMSG_FORMAT_NEG;
        info.u.fmt.codec_id       = inlink->codec;
        info.u.fmt.sample_fmt     = inlink->format;
        info.u.fmt.sample_rate    = inlink->sample_rate;
        info.u.fmt.order_channels = inlink->ch_layout.order;
        info.u.fmt.nb_channels    = inlink->ch_layout.nb_channels;

        ret = send(priv->fd, &info, sizeof(info), 0);
        if (ret < 0)
            return AVERROR(errno);
    }

    return 0;
}

static int rpmsgsink_query_formats(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterFormats *codecs = NULL;
    const AVCodec *codec = NULL;
    void *iterate = NULL;
    enum AVCodecID id;
    int ret, dup = 0;

    while (codec = av_codec_iterate(&iterate)) {
        if (codec->type != inlink->type || av_codec_is_encoder(codec))
            continue;

        id = codec->id;
        if (avcodec_is_pcm_lossless(id)) {
            if (!dup) {
                id = AV_CODEC_ID_RAWAUDIO;
                dup = 1;
            } else {
                continue;
            }
        }

        if (ret = ff_add_format(&codecs, id) < 0)
            goto error;
    }

    ret = ff_set_common_codecs(ctx, codecs);
    if (ret < 0)
        goto error;

    ret = ff_set_common_formats(ctx, ff_all_formats(AVMEDIA_TYPE_AUDIO));
    if (ret < 0)
        goto error;

    ret = ff_set_common_all_channel_counts(ctx);
    if (ret < 0)
        goto error;

    ret = ff_set_common_all_samplerates(ctx);
    if (ret < 0)
        goto error;

    return 0;

error:
    if (codecs && !codecs->refcount)
        ff_formats_unref(&codecs);

    return ret;
}

static bool rpmsgsink_handle_connected(AVFilterContext *ctx)
{
    RpmsgSinkContext *priv = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    int fd;

    fd = accept(priv->fd, NULL, NULL);
    if (fd < 0)
        return false;

    close(priv->fd);
    priv->fd = fd;

    if (inlink->init_state == AVLINK_INIT)
        return rpmsgsink_config_props(inlink) >= 0;

    return true;
}

static int rpmsgsink_handle_message(AVFilterContext *ctx)
{
    RpmsgSinkContext *priv = ctx->priv;
    RpmsgInfo info;
    ssize_t ret;

    ret = recv(priv->fd, &info, sizeof(RpmsgInfo), 0);
    if (ret < 0)
        return AVERROR(errno);

    switch (info.flag) {
        case RPMSG_FRAME_REQ:
            priv->frame_request = true;
            ff_filter_set_ready(ctx, 100);
            break;

        case RPMSG_FRAME_EOF:
            ff_inlink_set_status(ctx->inputs[0], AVERROR_EOF);
            break;
    }

    return 0;
}

static int rpmsgsink_handle_frame(AVFilterContext *ctx)
{
    RpmsgSinkContext *priv = ctx->priv;
    ssize_t ret;

    ret = send(priv->fd, priv->bprint.str + priv->bprint.len -
               priv->remain_length, priv->remain_length, MSG_DONTWAIT);
    if (ret < 0)
        return AVERROR(errno);

    priv->remain_length -= ret;
    if (priv->remain_length == 0)
        av_bprint_finalize(&priv->bprint, NULL);

    return 0;
}

static int rpmsgsink_process_command(AVFilterContext *ctx, const char *cmd, const char *args,
                                     char *res, int res_len, int flags)
{
    RpmsgSinkContext *priv = ctx->priv;
    struct pollfd *pollfd;
    int ret = 0;

    if (!strcmp(cmd, "get_pollfd")) {
        pollfd = (struct pollfd *)res;

        if (!res || res_len < sizeof(struct pollfd))
            return AVERROR(EINVAL);

        pollfd->fd = priv->fd;
        pollfd->events = POLLIN;

        if (priv->remain_length > 0)
            pollfd->events |= POLLOUT;

        return 1;
    } else if (!strcmp(cmd, "poll_available")) {
        pollfd = (struct pollfd *)res;

        if (pollfd->revents & POLLIN) {
            if (priv->is_connected)
                ret = rpmsgsink_handle_message(ctx);
            else
                priv->is_connected = rpmsgsink_handle_connected(ctx);
        }

        if (pollfd->revents & POLLOUT)
            ret = rpmsgsink_handle_frame(ctx);

        if (ret < 0 || (pollfd->revents & POLLHUP)) {
            rpmsgsink_uninit(ctx);
            return rpmsgsink_init(ctx);
        }

        return 0;
    }

    return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
}

static void rpmsgsink_serialize_frame(AVBPrint *bprint, const AVFrame *src)
{
    int i;

    av_bprint_append_data(bprint, (char *)src, sizeof(AVFrame));

    if (av_sample_fmt_is_planar(src->format))
        for (i = 0; i < src->ch_layout.nb_channels; i++)
            av_bprint_append_data(bprint, src->extended_data[i], src->linesize[0]);
    else
        av_bprint_append_data(bprint, src->extended_data[0], src->linesize[0]);
}

static int rpmsgsink_activate(AVFilterContext *ctx)
{
    AVFilterLink *inlink = ctx->inputs[0];
    RpmsgSinkContext *priv = ctx->priv;
    RpmsgInfo info;
    AVFrame *frame;
    int64_t pts;
    ssize_t ret;

    ff_inlink_acknowledge_status(inlink, &ret, &pts);
    if (ret >= 0 && priv->frame_request) {
        if(!ff_inlink_check_available_frame(inlink))
           ff_inlink_request_frame(inlink);
        priv->frame_request = false;
    } else if (ret == AVERROR_EOF) {
        info.flag = RPMSG_FRAME_EOF;
        ret = send(priv->fd, &info, sizeof(RpmsgInfo), 0);
        if (ret < 0)
            return AVERROR(errno);

        av_bprint_finalize(&priv->bprint, NULL);
        priv->frame_request = false;
        priv->remain_length = 0;
    }

    if (priv->remain_length ||
        !ff_inlink_check_available_frame(inlink))
        return 0;

    ret = ff_inlink_consume_frame(inlink, &frame);
    if (ret < 0)
        return ret;

    av_bprint_init(&priv->bprint, 0, AV_BPRINT_SIZE_UNLIMITED);
    rpmsgsink_serialize_frame(&priv->bprint, frame);
    av_frame_free(&frame);

    info.flag         = RPMSG_FRAME_ACK;
    info.u.frm.length = priv->bprint.len;
    info.u.frm.offset = 0;
    ret = send(priv->fd, &info, sizeof(info), 0);
    if (ret < 0)
        return AVERROR(errno);

    ret = send(priv->fd, priv->bprint.str, priv->bprint.len, MSG_DONTWAIT);
    if (ret != priv->bprint.len && ret >= 0) {
        priv->remain_length = priv->bprint.len - ret;
        return FFERROR_NOT_READY;
    }

    av_bprint_finalize(&priv->bprint, NULL);

    return 0;
}

#define OFFSET(x) offsetof(RpmsgSinkContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM

static const AVOption rpmsgsink_options[] = {
    { "rp_name", "Rpmsg socket name", OFFSET(rp_name), AV_OPT_TYPE_STRING, {.str = NULL }, .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(rpmsgsink);

static const AVFilterPad rpmsgsink_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = rpmsgsink_config_props,
    },
};

const AVFilter ff_asink_rpmsgsink = {
    .name            = "rpmsgsink",
    .description     = NULL_IF_CONFIG_SMALL("Transform audio to another core."),
    .priv_size       = sizeof(RpmsgSinkContext),
    .init            = rpmsgsink_init,
    .uninit          = rpmsgsink_uninit,
    FILTER_INPUTS(rpmsgsink_inputs),
    .outputs         = NULL,
    .activate        = rpmsgsink_activate,
    FILTER_QUERY_FUNC(rpmsgsink_query_formats),
    .process_command = rpmsgsink_process_command,
    .priv_class      = &rpmsgsink_class,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
