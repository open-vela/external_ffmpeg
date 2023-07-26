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

#include "packet_wrapper.h"

static void wrap_free_packet(void *opaque, uint8_t *data)
{
    AVPacket *packet = (AVPacket *)data;

    av_packet_free(&packet);
}

static void wrap_free_params(void *opaque, uint8_t *data)
{
    AVCodecParameters *params = (AVCodecParameters *)data;

    avcodec_parameters_free(&params);
}

AVFrame* wrap_frame(AVPacket* packet, AVCodecParameters* params)
{
    AVFrame *frame = NULL;
    int ret;

    frame = av_frame_alloc();
    if (!frame)
        return NULL;

    /* Packet field. */
    frame->buf[0] = av_buffer_create((void *)packet,
                                     sizeof(AVPacket) + AV_INPUT_BUFFER_PADDING_SIZE,
                                     wrap_free_packet, NULL, 0);
    if (!frame->buf[0])
        goto failed;

    frame->linesize[0] = frame->buf[0]->size;
    frame->data[0]     = frame->buf[0]->data;

    /* Paramters field. */
    if (params) {
        frame->opaque_ref = av_buffer_create((void *)params,
                                             sizeof(AVCodecParameters) + AV_INPUT_BUFFER_PADDING_SIZE,
                                             wrap_free_params, NULL, 0);
        if (!frame->opaque_ref)
            goto failed;
    }

    frame->pts = packet->pts;
    return frame;

failed:
    av_frame_free(&frame);
    return NULL;
}

void unwrap_frame(AVFrame* frame, AVPacket** packetptr, AVCodecParameters** paramsptr)
{
    if (packetptr) {
        if (frame->linesize[0])
            *packetptr = (AVPacket *)frame->data[0];
        else
            *packetptr = NULL;
    }

    if (paramsptr) {
        if (frame->opaque_ref)
            *paramsptr = (AVCodecParameters *)frame->opaque_ref->data;
        else
            *paramsptr = NULL;
    }
}
