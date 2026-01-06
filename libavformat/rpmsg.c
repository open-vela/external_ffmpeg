/*
 * Rpmsg socket protocol
 *
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
 *
 * Rpmsg socket url_protocol
 */

#include "libavutil/parseutils.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "os_support.h"
#include "network.h"
#include <netpacket/rpmsg.h>
#include "url.h"

typedef struct RpmsgContext {
    const AVClass *class;
    struct sockaddr_rpmsg addr;
    int timeout;
    int listen;
    int type;
    int fd;
    int pkt_size;
} RpmsgContext;

#define OFFSET(x) offsetof(RpmsgContext, x)
#define ED AV_OPT_FLAG_DECODING_PARAM|AV_OPT_FLAG_ENCODING_PARAM
static const AVOption rpmsg_options[] = {
    { "listen",    "Open socket for listening",             OFFSET(listen),  AV_OPT_TYPE_BOOL,  { .i64 = 0 },                    0,       1, ED },
    { "timeout",   "Timeout in ms",                         OFFSET(timeout), AV_OPT_TYPE_INT,   { .i64 = -1 },                  -1, INT_MAX, ED },
    { "type",      "Socket type",                           OFFSET(type),    AV_OPT_TYPE_INT,   { .i64 = SOCK_STREAM },    INT_MIN, INT_MAX, ED, "type" },
    { "stream",    "Stream (reliable stream-oriented)",     0,               AV_OPT_TYPE_CONST, { .i64 = SOCK_STREAM },    INT_MIN, INT_MAX, ED, "type" },
    { "datagram",  "Datagram (unreliable packet-oriented)", 0,               AV_OPT_TYPE_CONST, { .i64 = SOCK_DGRAM },     INT_MIN, INT_MAX, ED, "type" },
    { "seqpacket", "Seqpacket (reliable packet-oriented",   0,               AV_OPT_TYPE_CONST, { .i64 = SOCK_SEQPACKET }, INT_MIN, INT_MAX, ED, "type" },
    { "pkt_size",  "Maximum packet size",                   OFFSET(pkt_size), AV_OPT_TYPE_INT,  { .i64 = 0 },              0, INT_MAX, ED },
    { NULL }
};

static const AVClass rpmsg_class = {
    .class_name = "rpmsg",
    .item_name  = av_default_item_name,
    .option     = rpmsg_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int rpmsg_open(URLContext *h, const char *filename, int flags)
{
    RpmsgContext *s = h->priv_data;
    int name_size, cpu_size;
    const char *opts, *cpu;
    int fd, ret;
    char buf[8];

    av_strstart(filename, "rpmsg:", &filename);
    s->addr.rp_family = AF_RPMSG;

    name_size = RPMSG_SOCKET_NAME_SIZE;
    cpu_size  = RPMSG_SOCKET_CPU_SIZE;
    cpu  = strchr(filename, ':');
    opts = strrchr(filename, '?');

    if (opts) {
        if (av_find_info_tag(buf, sizeof(buf), "listen", opts + 1))
            s->listen = strtol(buf, NULL, 10);
    }

    if (cpu) {
        if (opts)
            cpu_size = opts - cpu;

        name_size = cpu - filename + 1;
        av_strlcpy(s->addr.rp_cpu, cpu + 1, FFMIN(cpu_size, RPMSG_SOCKET_CPU_SIZE));
    } else {
        if (opts)
            name_size = opts - filename + 1;
    }

    av_strlcpy(s->addr.rp_name, filename, FFMIN(name_size, RPMSG_SOCKET_NAME_SIZE));

    if ((fd = ff_socket(AF_RPMSG, s->type, 0, h)) < 0)
        return ff_neterrno();

    if (s->timeout < 0 && h->rw_timeout)
        s->timeout = h->rw_timeout / 1000;

    if (s->listen) {
        ret = ff_listen_bind(fd, (struct sockaddr *)&s->addr,
                             sizeof(s->addr), s->timeout, h);
        if (ret < 0)
            goto fail;
        fd = ret;
    } else {
        ret = ff_listen_connect(fd, (struct sockaddr *)&s->addr,
                                sizeof(s->addr), s->timeout, h, 0);
        if (ret < 0)
            goto fail;
    }

    s->fd = fd;
    h->max_packet_size = s->pkt_size;

    return 0;

fail:
    if (fd >= 0)
        closesocket(fd);
    return ret;
}

static int rpmsg_read(URLContext *h, uint8_t *buf, int size)
{
    RpmsgContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd(s->fd, 0);
        if (ret < 0)
            return ret;
    }
    ret = recv(s->fd, buf, size, 0);
    if (!ret && s->type == SOCK_STREAM)
        return AVERROR_EOF;
    return ret < 0 ? ff_neterrno() : ret;
}

static int rpmsg_write(URLContext *h, const uint8_t *buf, int size)
{
    RpmsgContext *s = h->priv_data;
    int ret;

    if (!(h->flags & AVIO_FLAG_NONBLOCK)) {
        ret = ff_network_wait_fd(s->fd, 1);
        if (ret < 0)
            return ret;
    }
    ret = send(s->fd, buf, size, MSG_NOSIGNAL);
    return ret < 0 ? ff_neterrno() : ret;
}

static int rpmsg_close(URLContext *h)
{
    RpmsgContext *s = h->priv_data;
    closesocket(s->fd);
    return 0;
}

static int rpmsg_get_file_handle(URLContext *h)
{
    RpmsgContext *s = h->priv_data;
    return s->fd;
}

const URLProtocol ff_rpmsg_protocol = {
    .name                = "rpmsg",
    .url_open            = rpmsg_open,
    .url_read            = rpmsg_read,
    .url_write           = rpmsg_write,
    .url_close           = rpmsg_close,
    .url_get_file_handle = rpmsg_get_file_handle,
    .priv_data_size      = sizeof(RpmsgContext),
    .priv_data_class     = &rpmsg_class,
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
};
