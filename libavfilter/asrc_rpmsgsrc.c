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
 * Receive audio data from another core by rpmsg socket
 */

#include <netpacket/rpmsg.h>
#include <poll.h>

#include "libavformat/network.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avstring.h"
#include "packet_wrapper.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "filters.h"
#include "audio.h"
#include "rpmsg.h"

typedef struct RpmsgSrcContext {
    const             AVClass *class;

    int               fd;
    bool              is_connected;
    RpmsgInfo         format_info;
    RpmsgInfo         frame_info;

    char              *rp_cpu;
    char              *rp_name;
    AVFrame           *frame;
    AVCodecParameters *param;
    AVPacket          *pkt;
} RpmsgSrcContext;

static av_cold void reinit_resource(AVFilterContext *ctx)
{
    RpmsgSrcContext *priv = ctx->priv;

    av_packet_free(&priv->pkt);
    avcodec_parameters_free(&priv->param);
    av_frame_free(&priv->frame);
    memset(&priv->format_info, 0, sizeof(priv->format_info));
    memset(&priv->frame_info, 0, sizeof(priv->frame_info));
}

static int rpmsgsrc_handle_message_header(AVFilterContext *ctx)
{
    RpmsgSrcContext *priv = ctx->priv;
    RpmsgInfo info;
    uint32_t flag;
    ssize_t ret;

    ret = recv(priv->fd, &info, sizeof(info), 0);
    if (ret < 0)
        return AVERROR(errno);

    switch (info.flag) {
        case RPMSG_FRAME_EOF:
            reinit_resource(ctx);
            ff_avfilter_link_set_in_status(ctx->outputs[0], AVERROR_EOF, AV_NOPTS_VALUE);
            break;

        case RPMSG_FORMAT_NEG:
            priv->format_info = info;

            avfilter_graph_reconfig(ctx->graph, ctx);

            priv->frame_info.flag = RPMSG_FRAME_REQ;
            ret = send(priv->fd, &priv->frame_info, sizeof(priv->frame_info), 0);
            if (ret < 0)
                return AVERROR(errno);
            break;

        case RPMSG_FRAME_ACK:
            priv->frame_info = info;
            break;
    }

    return 0;
}

static int rpmsgsrc_handle_compress_data(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    RpmsgSrcContext *priv = ctx->priv;
    uint8_t *tmp = NULL;
    size_t length;
    size_t offset;
    ssize_t ret;

    if (!priv->pkt) {
        priv->pkt = av_packet_alloc();
        if (!priv->pkt)
            return AVERROR(ENOMEM);
    }

    if (priv->frame_info.u.frm.offset < sizeof(AVPacket)) {

        /* deserialize packet fields */
        ret = recv(priv->fd, (uint8_t *)priv->pkt + priv->frame_info.u.frm.offset,
                   sizeof(AVPacket) - priv->frame_info.u.frm.offset, MSG_DONTWAIT);
        if (ret < 0)
            return ret;

        priv->pkt->side_data_elems = 0;
        priv->pkt->side_data = NULL;
        priv->frame_info.u.frm.offset += ret;
    } else if (priv->frame_info.u.frm.offset < sizeof(AVPacket) + priv->pkt->size) {

        // Allocate and copy packet data
        tmp = av_mallocz(priv->pkt->size);
        offset = priv->frame_info.u.frm.offset - sizeof(AVPacket);
        length = priv->pkt->size - offset;
        ret = recv(priv->fd, tmp + offset, length, MSG_DONTWAIT);
        if (ret < 0)
            goto error;

        priv->frame_info.u.frm.offset += ret;
        if (priv->frame_info.u.frm.offset == sizeof(AVPacket) + priv->pkt->size) {
            /* create pkt that can use citation technology */
            priv->pkt->data = tmp;
            priv->pkt->buf = NULL;
            if (av_packet_make_refcounted(priv->pkt) < 0) {
                ret = AVERROR(ENOMEM);
                goto error;
            }
            av_freep(&tmp);
        }
    } else if (priv->frame_info.u.frm.offset <
               sizeof(AVPacket) + priv->pkt->size + sizeof(AVCodecParameters)) {

        /* deserialize codec parameters */
        if (!priv->param) {
            priv->param = avcodec_parameters_alloc();
            if (!priv->param)
                return AVERROR(ENOMEM);
        }

        /* deserialize codec parameters fields */
        offset = priv->frame_info.u.frm.offset - sizeof(AVPacket) - priv->pkt->size;
        length = sizeof(AVCodecParameters) - offset;
        ret = recv(priv->fd, (uint8_t *)priv->param + offset, length, MSG_DONTWAIT);
        if (ret < 0)
            return ret;

        priv->frame_info.u.frm.offset += ret;
    } else if (priv->param->extradata_size && priv->frame_info.u.frm.offset ==
               sizeof(AVPacket) + priv->pkt->size + sizeof(AVCodecParameters)) {

            /* allocate and copy extradata */
            priv->param->extradata = av_mallocz(priv->param->extradata_size);
            offset = priv->frame_info.u.frm.offset - sizeof(AVPacket) -
                     priv->pkt->size - sizeof(AVCodecParameters);
            length = priv->param->extradata_size - offset;
            ret = recv(priv->fd, priv->param->extradata + offset, length, MSG_DONTWAIT);
            if (ret < 0)
                return ret;

            priv->frame_info.u.frm.offset += ret;
    }

    if (priv->frame_info.u.frm.offset == priv->frame_info.u.frm.length) {
        priv->frame = wrap_frame(priv->pkt, priv->param, NULL);
        priv->frame->format = outlink->format;
        priv->frame->sample_rate = outlink->sample_rate;

        av_channel_layout_copy(&priv->frame->ch_layout, &outlink->ch_layout);
        ff_filter_set_ready(ctx, 100);
    }

    return 0;

error:
    av_freep(&tmp);
    return ret;
}

static int rpmsgsrc_handle_raw_data(AVFilterContext *ctx)
{
    RpmsgSrcContext *priv = ctx->priv;
    size_t length;
    size_t offset;
    ssize_t ret;
    int i;

    if (priv->frame_info.u.frm.offset < sizeof(AVFrame)) {
        ret = recv(priv->fd, (uint8_t *)priv->frame + priv->frame_info.u.frm.offset,
                   sizeof(AVFrame) - priv->frame_info.u.frm.offset, MSG_DONTWAIT);
        if (ret < 0)
            return AVERROR(errno);

        priv->frame_info.u.frm.offset += ret;

        if (priv->frame_info.u.frm.offset == sizeof(AVFrame)) {
            ret = av_frame_get_buffer(priv->frame, 0);
            if (ret < 0)
                return ret;
        }
    } else if (av_sample_fmt_is_planar(priv->frame->format)) {
        i = (priv->frame_info.u.frm.offset - sizeof(AVFrame)) / priv->frame->linesize[0];
        for (; i < priv->frame->ch_layout.nb_channels; i++) {
            offset = priv->frame_info.u.frm.offset - i * priv->frame->linesize[0] - sizeof(AVFrame);
            length = priv->frame->linesize[0] - offset;
            ret = recv(priv->fd, priv->frame->extended_data[i] + offset, length, MSG_DONTWAIT);
            if (ret < 0)
                return AVERROR(errno);

            priv->frame_info.u.frm.offset += ret;

            if(ret < length)
               break;
        }
    } else {
        offset = priv->frame_info.u.frm.offset - sizeof(AVFrame);
        length = priv->frame->linesize[0] - offset;
        ret = recv(priv->fd, priv->frame->extended_data[0] + offset, length, MSG_DONTWAIT);
        if (ret < 0)
            return AVERROR(errno);

        priv->frame_info.u.frm.offset += ret;
    }

    if (priv->frame_info.u.frm.offset == priv->frame_info.u.frm.length)
        ff_filter_set_ready(ctx, 100);

    return 0;
}

static int rpmsgsrc_query_formats(AVFilterContext *ctx)
{
    AVFilterChannelLayouts *layout = NULL;
    RpmsgSrcContext *priv = ctx->priv;
    AVFilterFormats *formats = NULL;
    AVFilterFormats *codecs = NULL;
    AVChannelLayout ch_layout;
    int ret;

    if (priv->format_info.flag != RPMSG_FORMAT_NEG)
        return FFERROR_NOT_READY;

    av_channel_layout_default(&ch_layout, priv->format_info.u.fmt.nb_channels);
    ch_layout.order = priv->format_info.u.fmt.order_channels;

    if ((ret = ff_add_format(&formats, priv->format_info.u.fmt.sample_fmt)) < 0 ||
        (ret = ff_set_common_formats(ctx, formats)) < 0 ||
        (ret = ff_add_channel_layout(&layout, &ch_layout)) < 0 ||
        (ret = ff_set_common_channel_layouts(ctx, layout)) < 0 ||
        (ret = ff_add_format(&codecs, priv->format_info.u.fmt.codec_id)) < 0 ||
        (ret = ff_set_common_codecs(ctx, codecs)) < 0)
        return ret;

    formats = NULL;
    if ((ret = ff_add_format(&formats, priv->format_info.u.fmt.sample_rate)) < 0)
        return ret;

    return ff_set_common_samplerates(ctx, formats);
}

static av_cold int rpmsgsrc_props(AVFilterLink *outlink)
{
    return 0;
}

static av_cold int rpmsgsrc_init(AVFilterContext *ctx)
{
    RpmsgSrcContext *priv = ctx->priv;
    struct sockaddr_rpmsg addr;
    int ret;

    if (priv->rp_name == NULL || priv->rp_cpu == NULL)
        return AVERROR(EINVAL);

    priv->fd = socket(PF_RPMSG, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (priv->fd < 0)
        return AVERROR(errno);

    addr.rp_family = AF_RPMSG;
    av_strlcpy(addr.rp_name, priv->rp_name, RPMSG_SOCKET_NAME_SIZE);
    av_strlcpy(addr.rp_cpu, priv->rp_cpu, RPMSG_SOCKET_CPU_SIZE);

    ret = connect(priv->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS)
        return AVERROR(errno);

    priv->is_connected = ret >= 0;

    if (ff_socket_nonblock(priv->fd, 0) < 0)
        return AVERROR(errno);

    return 0;
}

static av_cold void rpmsgsrc_uninit(AVFilterContext *ctx)
{
    RpmsgSrcContext *priv = ctx->priv;

    reinit_resource(ctx);
    close(priv->fd);
}

static int rpmsgsrc_process_command(AVFilterContext *ctx,const char *cmd, const char *args,
                                    char *res, int res_len, int flags)
{
    RpmsgSrcContext *priv = ctx->priv;
    struct pollfd *pollfd;
    int ret = 0;

    if (!strcmp(cmd, "get_pollfd")) {
        pollfd = (struct pollfd *)res;

        if (!res || res_len < sizeof(struct pollfd))
            return AVERROR(EINVAL);

        pollfd->fd = priv->fd;
        pollfd->events = POLLIN;

        if (!priv->is_connected)
            pollfd->events |= POLLOUT;

        return 1;
    } else if (!strcmp(cmd, "poll_available")) {
        pollfd = (struct pollfd *)res;

        if (pollfd->revents & POLLOUT)
            priv->is_connected = true;

        if (pollfd->revents & POLLIN) {
            if (!priv->frame) {
                priv->frame = av_frame_alloc();
                if (!priv->frame)
                    return AVERROR(ENOMEM);
            } else if (priv->frame_info.u.frm.offset == priv->frame_info.u.frm.length)
                ret = rpmsgsrc_handle_message_header(ctx);
            else if (avcodec_is_pcm_lossless(priv->format_info.u.fmt.codec_id))
                ret = rpmsgsrc_handle_raw_data(ctx);
            else
                ret = rpmsgsrc_handle_compress_data(ctx);
        }

        if (ret < 0 || (pollfd->revents & POLLHUP)) {
            rpmsgsrc_uninit(ctx);
            return rpmsgsrc_init(ctx);
        }

        return 0;
    }

    return ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
}

static int rpmsgsrc_activate(AVFilterContext *ctx)
{
    AVFilterLink *outlink = ctx->outputs[0];
    RpmsgSrcContext *priv = ctx->priv;
    int offset;
    int ret;

    ret = ff_outlink_get_status(outlink);
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            priv->frame_info.flag = RPMSG_FRAME_EOF;
            ret = send(priv->fd, &priv->frame_info, sizeof(priv->frame_info), 0);
            if (ret < 0)
                return AVERROR(errno);
        }

        reinit_resource(ctx);
        return ret;
    }

    if (priv->frame && priv->frame_info.u.frm.offset == priv->frame_info.u.frm.length) {

        ret = ff_filter_frame(outlink, priv->frame);

        priv->pkt = NULL;
        priv->param = NULL;
        priv->frame = NULL;

        return ret;
    } else if (ff_outlink_frame_wanted(outlink)) {
        priv->frame_info.flag = RPMSG_FRAME_REQ;
        ret = send(priv->fd, &priv->frame_info, sizeof(RpmsgFrameInfo), 0);
        if (ret < 0)
            return ret;
    }

    return 0;
}

#define OFFSET(x) offsetof(RpmsgSrcContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_AUDIO_PARAM

static const AVOption rpmsgsrc_options[] = {
    { "rp_cpu",  "Remote cpu name",   OFFSET(rp_cpu),  AV_OPT_TYPE_STRING, {.str = NULL }, .flags = FLAGS },
    { "rp_name", "Rpmsg socket name", OFFSET(rp_name), AV_OPT_TYPE_STRING, {.str = NULL }, .flags = FLAGS },
    { NULL },
};

AVFILTER_DEFINE_CLASS(rpmsgsrc);

static const AVFilterPad rpmsgsrc_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = rpmsgsrc_props,
    },
};

const AVFilter ff_asrc_rpmsgsrc = {
    .name            = "rpmsgsrc",
    .description     = NULL_IF_CONFIG_SMALL("Receive audio from another core."),
    .priv_size       = sizeof(RpmsgSrcContext),
    .init            = rpmsgsrc_init,
    .uninit          = rpmsgsrc_uninit,
    FILTER_OUTPUTS(rpmsgsrc_outputs),
    FILTER_QUERY_FUNC(rpmsgsrc_query_formats),
    .process_command = rpmsgsrc_process_command,
    .priv_class      = &rpmsgsrc_class,
    .inputs          = NULL,
    .activate        = rpmsgsrc_activate,
    .flags           = AVFILTER_FLAG_SUPPORT_POLL,
};
