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

#ifndef AVFILTER_PACKETQUEUE_H
#define AVFILTER_PACKETQUEUE_H

/**
 * FFPacketQueue: simple AVPacket queue API
 *
 * Note: this API is not thread-safe. Concurrent access to the same queue
 * must be protected by a mutex or any synchronization mechanism.
 */

#include "libavcodec/packet.h"
#include "libavutil/mem.h"

typedef struct FFPacketBucket {
    AVPacket *packet;
} FFPacketBucket;

/**
 * Structure to hold global options and statistics for packet queues.
 *
 * This structure is intended to allow implementing global control of the
 * packet queues, including memory consumption caps.
 *
 * It is currently empty.
 */
typedef struct FFPacketQueueGlobal {
    char dummy; /* C does not allow empty structs */
} FFPacketQueueGlobal;

/**
 * Queue of AVPacket pointers.
 */
typedef struct FFPacketQueue {

    /**
     * Array of allocated buckets, used as a circular buffer.
     */
    FFPacketBucket *queue;

    /**
     * Size of the array of buckets.
     */
    size_t allocated;

    /**
     * Tail of the queue.
     * It is the index in the array of the next packet to take.
     */
    size_t tail;

    /**
     * Number of currently queued packets.
     */
    size_t queued;

    /**
     * Pre-allocated bucket for queues of size 1.
     */
    FFPacketBucket first_bucket;

    /**
     * Total number of packets entered in the queue.
     */
    uint64_t total_packets_head;

    /**
     * Total number of packets dequeued from the queue.
     * queued = total_packets_head - total_packets_tail
     */
    uint64_t total_packets_tail;

    /**
     * Total number of size entered in the queue.
     */
    uint64_t total_size_head;

    /**
     * Total number of size dequeued from the queue.
     * queued_size = total_size_head - total_size_tail
     */
    uint64_t total_size_tail;

} FFPacketQueue;

/**
 * Init a global structure.
 */
void ff_packetqueue_global_init(FFPacketQueueGlobal *fqg);

/**
 * Init a packet queue and attach it to a global structure.
 */
void ff_packetqueue_init(FFPacketQueue *pq, FFPacketQueueGlobal *fqg);

/**
 * Free the queue and all queued packets.
 */
void ff_packetqueue_free(FFPacketQueue *pq);

/**
 * Add a packet.
 * @return  >=0 or an AVERROR code.
 */
int ff_packetqueue_add(FFPacketQueue *pq, AVPacket *packet);

/**
 * Take the first packet in the queue.
 * Must not be used with empty queues.
 */
AVPacket *ff_packetqueue_take(FFPacketQueue *pq);

/**
 * Access a packet in the queue, without removing it.
 * The first packet is numbered 0; the designated packet must exist.
 */
AVPacket *ff_packetqueue_peek(FFPacketQueue *pq, size_t idx);

/**
 * Get the number of queued packets.
 */
static inline size_t ff_packetqueue_queued_packets(const FFPacketQueue *pq)
{
    return pq->queued;
}

#endif /* AVFILTER_PACKETQUEUE_H */
