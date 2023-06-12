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

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "libavutil/pixdesc.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavformat/avformat.h"
#include "libavformat/mux.h"
#include "fbdev_common.h"
#include "avdevice.h"

typedef struct FBDevOverlayContext{
    AVClass *class;                 ///< class for private options
    int fd;                         ///< framebuffer device file descriptor
    bool stopped;                   ///< frame play status
    uint8_t *data;                  ///< framebuffer data
    struct fb_overlayinfo_s oinfo;
} FBDevOverlayContext;

#define  FBOVERLAY_NUMBER 2

static av_cold int fbdev_overlay_write_header(AVFormatContext *h)
{
    FBDevOverlayContext *dev_ctx = h->priv_data;
    AVCodecParameters *par = h->streams[0]->codecpar;
    struct fb_videoinfo_s vinfo;
    int ret, flags = O_RDWR;
    const char* device;

    if (dev_ctx->stopped)
        return AVERROR_EOF;

    if (h->nb_streams != 1 || h->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
        av_log(h, AV_LOG_ERROR, "Only a single video stream is supported.\n");
        return AVERROR(EINVAL);
    }

    if (h->url[0])
        device = h->url;
    else
        device = ff_fbdev_default_device();

    if ((dev_ctx->fd = avpriv_open(device, flags)) < 0) {
        av_log(h, AV_LOG_ERROR,
               "Could not open framebuffer device '%s': %s.\n",
               device, av_err2str(ret));
        return AVERROR(errno);
    }

    ret = ioctl(dev_ctx->fd, FBIOGET_VIDEOINFO, &vinfo);
    if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "Unable to get video information.\n");
        goto fail;
    }

    ret = ioctl(dev_ctx->fd, FBIO_SELECT_OVERLAY, dev_ctx->oinfo.overlay);
    if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "Unable to select overlay: %d.\n", dev_ctx->oinfo.overlay);
        goto fail;
    }

    ret = ioctl(dev_ctx->fd, FBIOGET_OVERLAYINFO, &dev_ctx->oinfo);
    if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "Unable to get overlay information.\n");
        goto fail;
    }

    dev_ctx->data = mmap(NULL, dev_ctx->oinfo.fblen, PROT_READ | PROT_WRITE,
                         MAP_SHARED, dev_ctx->fd, 0);

    if (dev_ctx->data == MAP_FAILED) {
        av_log(h, AV_LOG_ERROR, "ioctl(FBIO_MMAP) failed: %d.\n", errno);
        goto fail;
    }

    dev_ctx->oinfo.sarea.x = 0;
    dev_ctx->oinfo.sarea.y = 0;
    dev_ctx->oinfo.sarea.w = par->width;
    dev_ctx->oinfo.sarea.h = par->height;

    if (par->width * vinfo.yres > par->height * vinfo.xres) {
        dev_ctx->oinfo.darea.w = vinfo.xres;
        dev_ctx->oinfo.darea.h = vinfo.xres * par->height / par->width;
        dev_ctx->oinfo.darea.x = 0;
        dev_ctx->oinfo.darea.y = (vinfo.yres - dev_ctx->oinfo.darea.h) / 2;
    } else {
        dev_ctx->oinfo.darea.h = vinfo.yres;
        dev_ctx->oinfo.darea.w = vinfo.yres * par->width / par->height;
        dev_ctx->oinfo.darea.x = (vinfo.xres - dev_ctx->oinfo.darea.w) / 2;
        dev_ctx->oinfo.darea.y = 0;
    }

    ret = ioctl(dev_ctx->fd, FBIOSET_AREA, &dev_ctx->oinfo);
    if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "Unable to set overlay select area.\n");
        goto fail;
    }

    ret = ioctl(dev_ctx->fd, FBIOSET_DESTAREA, &dev_ctx->oinfo);
    if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "Unable to set overlay display area.\n");
        goto fail;
    }

    return 0;
fail:
    if (dev_ctx->data) {
        munmap(dev_ctx->data, dev_ctx->oinfo.fblen);
        dev_ctx->data = NULL;
    }

    close(dev_ctx->fd);
    return AVERROR(errno);
}

static int fbdev_overlay_write_packet(AVFormatContext *h, AVPacket *pkt)
{
    FBDevOverlayContext *dev_ctx = h->priv_data;
    AVCodecParameters *par = h->streams[0]->codecpar;
    const uint8_t *in_y;
    const uint8_t *in_uv;
    uint8_t *row;
    int ret;
    int i;

    if (dev_ctx->stopped)
        return AVERROR_EOF;

    in_y  = pkt->data;
    in_uv = pkt->data + (par->width * par->height);

    dev_ctx->oinfo.yoffset += dev_ctx->oinfo.yres_virtual / FBOVERLAY_NUMBER;
    dev_ctx->oinfo.yoffset %= dev_ctx->oinfo.yres_virtual;

    row = dev_ctx->data + dev_ctx->oinfo.stride * dev_ctx->oinfo.yoffset * 3 / 2;
    for (i = 0; i < par->height; i++) {
        memcpy(row, in_y, par->width);
        in_y += par->width;
        row += dev_ctx->oinfo.stride;
    }

    row = dev_ctx->data + dev_ctx->oinfo.stride * dev_ctx->oinfo.yoffset * 3 / 2;
    row += dev_ctx->oinfo.stride * dev_ctx->oinfo.yres_virtual / FBOVERLAY_NUMBER;
    for (i = 0; i < par->height / 2; i++) {
        memcpy(row, in_uv, par->width);
        in_uv += par->width;
        row += dev_ctx->oinfo.stride;
    }

    ret = ioctl(dev_ctx->fd, FBIOPAN_OVERLAY, &dev_ctx->oinfo);
    if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "Unable to select pandisplayoverlay.\n");
        return AVERROR(errno);
    }

    return 0;
}

static av_cold int fbdev_overlay_write_trailer(AVFormatContext *h)
{
    FBDevOverlayContext *dev_ctx = h->priv_data;

    munmap(dev_ctx->data, dev_ctx->oinfo.fblen);
    close(dev_ctx->fd);

    return 0;
}

static int fbdev_overlay_get_device_list(AVFormatContext *s, AVDeviceInfoList *device_list)
{
    return ff_fbdev_get_device_list(device_list);
}

static int fbdev_overlay_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                               const char *key, int flags)
{
    AVDeviceCapabilitiesQuery *devcap = obj;
    AVFormatContext *h = devcap->device_context;
    FBDevOverlayContext *dev_ctx = h->priv_data;
    AVOptionRanges *ranges;
    struct fb_overlayinfo_s oinfo;
    enum AVPixelFormat pix_fmt;
    int ret = AVERROR(ENOMEM);
    int fd;

    fd = avpriv_open(h->url, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return AVERROR(errno);

    oinfo.overlay = dev_ctx->oinfo.overlay;

    ret = ioctl(fd, FBIO_SELECT_OVERLAY, oinfo.overlay);
    if (ret < 0) {
        close(fd);
        return AVERROR(errno);
    }

    if (ioctl(fd, FBIOGET_OVERLAYINFO, &oinfo) < 0) {
        close(fd);
        return AVERROR(errno);
    }

    close(fd);

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        goto err;

    if (!strcmp(key, "pixel_fmts")) {
        if(oinfo.color == FB_FMT_NV21)
            pix_fmt = AV_PIX_FMT_NV21;
        else {
            ret = AVERROR(EINVAL);
            goto err;
        }

        ranges->nb_components = 1;
        ranges->nb_ranges = 1;
        ranges->range = av_mallocz(sizeof(AVOptionRange *));
        if (!ranges->range)
            goto err;

        ranges->range[0] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[0])
            goto err;

        ranges->range[0]->is_range  = 0;
        ranges->range[0]->value_min = pix_fmt;
        ranges->range[0]->value_max = pix_fmt;
    } else
        goto err;

    *ranges_ = ranges;
    return ranges->nb_components;

err:
    av_opt_freep_ranges(&ranges);
    return AVERROR(errno);
}

static const AVClass fbdev_overlay_cap_class = {
    .class_name   = "fbdev_overlay outdev capbility",
    .item_name    = av_default_item_name,
    .version      = LIBAVUTIL_VERSION_INT,
    .category     = AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT,
    .query_ranges = fbdev_overlay_capbility_query_ranges,
};

static int fbdev_overlay_control_message(AVFormatContext *h, int type,
                                        void *data, size_t data_size)
{
    FBDevOverlayContext *dev_ctx = h->priv_data;

    switch (type) {
        case AV_APP_TO_DEV_GET_CAPS_REQUEST: {
            AVDeviceCapabilitiesQuery *caps = data;

            if (!caps)
                return AVERROR(EINVAL);

            caps->av_class = &fbdev_overlay_cap_class;
            caps->device_context = h;
            av_opt_set_defaults(caps);

            return 0;
        }
        case AV_APP_TO_DEV_START: {
            dev_ctx->stopped = false;
            avdevice_dev_to_app_control_message(h, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
            return 0;
        }
        case AV_APP_TO_DEV_STOP: {
            dev_ctx->stopped = true;
            avdevice_dev_to_app_control_message(h, AV_DEV_TO_APP_STATE_CHANGED, NULL, 0);
            return 0;
        }
    }

    return AVERROR(ENOSYS);
}

#define OFFSET(x) offsetof(FBDevOverlayContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption fbdev_overlay_options[] = {
    { "overlayno", "set overlay number of framebuffer", OFFSET(oinfo.overlay), AV_OPT_TYPE_INT, {.i64 = 0}, INT_MIN, INT_MAX, ENC },
    { NULL }
};

static const AVClass fbdev_overlay_class = {
    .class_name = "fbdev_overlay outdev",
    .item_name  = av_default_item_name,
    .option     = fbdev_overlay_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

const AVOutputFormat ff_fbdev_overlay_muxer = {
    .name                = "fbdev_overlay",
    .long_name           = NULL_IF_CONFIG_SMALL("NuttX framebuffer overlay"),
    .priv_data_size      = sizeof(FBDevOverlayContext),
    .audio_codec         = AV_CODEC_ID_NONE,
    .video_codec         = AV_CODEC_ID_RAWVIDEO,
    .write_header        = fbdev_overlay_write_header,
    .write_packet        = fbdev_overlay_write_packet,
    .write_trailer       = fbdev_overlay_write_trailer,
    .control_message     = fbdev_overlay_control_message,
    .get_device_list     = fbdev_overlay_get_device_list,
    .flags               = AVFMT_NOFILE | AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
    .priv_class          = &fbdev_overlay_class,
};
