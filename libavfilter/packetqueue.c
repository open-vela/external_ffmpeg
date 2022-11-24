/*
 * Generic packet queue
 * Copyright (c) 2022 Xiaomi Corporation
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with FFmpeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "packetqueue.h"

static inline FFPacketBucket *bucket(FFPacketQueue *pq, size_t idx)
{
    return &pq->queue[(pq->tail + idx) & (pq->allocated - 1)];
}

void ff_packetqueue_global_init(FFPacketQueueGlobal *pqg)
{
}

static void check_consistency(FFPacketQueue *pq)
{
#if defined(ASSERT_LEVEL) && ASSERT_LEVEL >= 2
    uint64_t size = 0;
    size_t i;

    av_assert0(pq->queued == pq->total_packets_head - pq->total_packets_tail);
    for (i = 0; i < pq->queued; i++)
        size += bucket(pq, i)->packet->size;
    av_assert0(size == pq->total_size_head - pq->total_size_tail);
#endif
}

void ff_packetqueue_init(FFPacketQueue *pq, FFPacketQueueGlobal *pqg)
{
    pq->queue = &pq->first_bucket;
    pq->allocated = 1;
}

void ff_packetqueue_free(FFPacketQueue *pq)
{
    while (pq->queued) {
        AVPacket *packet = ff_packetqueue_take(pq);
        av_packet_free(&packet);
    }
    if (pq->queue != &pq->first_bucket)
        av_freep(&pq->queue);
}

int ff_packetqueue_add(FFPacketQueue *pq, AVPacket *packet)
{
    FFPacketBucket *b;

    check_consistency(pq);
    if (pq->queued == pq->allocated) {
        if (pq->allocated == 1) {
            size_t na = 8;
            FFPacketBucket *nq = av_realloc_array(NULL, na, sizeof(*nq));
            if (!nq)
                return AVERROR(ENOMEM);
            nq[0] = pq->queue[0];
            pq->queue = nq;
            pq->allocated = na;
        } else {
            size_t na = pq->allocated << 1;
            FFPacketBucket *nq = av_realloc_array(pq->queue, na, sizeof(*nq));
            if (!nq)
                return AVERROR(ENOMEM);
            if (pq->tail + pq->queued > pq->allocated)
                memmove(nq + pq->allocated, nq,
                        (pq->tail + pq->queued - pq->allocated) * sizeof(*nq));
            pq->queue = nq;
            pq->allocated = na;
        }
    }
    b = bucket(pq, pq->queued);
    b->packet = packet;
    pq->queued++;
    pq->total_packets_head++;
    pq->total_size_head += packet->size;
    check_consistency(pq);

    return 0;
}

AVPacket *ff_packetqueue_take(FFPacketQueue *pq)
{
    FFPacketBucket *b;

    check_consistency(pq);
    av_assert1(pq->queued);
    b = bucket(pq, 0);
    pq->queued--;
    pq->tail++;
    pq->tail &= pq->allocated - 1;
    pq->total_packets_tail++;
    pq->total_size_tail += b->packet->size;
    check_consistency(pq);

    return b->packet;
}

AVPacket *ff_packetqueue_peek(FFPacketQueue *pq, size_t idx)
{
    FFPacketBucket *b;

    check_consistency(pq);
    av_assert1(idx < pq->queued);
    b = bucket(pq, idx);
    check_consistency(pq);
    return b->packet;
}
