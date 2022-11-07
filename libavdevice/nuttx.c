/*
 * NUTTX input and output
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
 * NUTTX input and output: common code
 */

#include "libavcodec/codec_id.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avstring.h"

#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

#include "nuttx.h"

static int ff_nuttx_samplerate_convert(int samplerate, int *sample_rates, int num)
{
    int i;

    for (i = 0; i < num && samplerate; i++) {
        if (samplerate & AUDIO_SAMP_RATE_8K) {
            samplerate &= ~AUDIO_SAMP_RATE_8K;
            sample_rates[i] = 8000;
        } else if (samplerate & AUDIO_SAMP_RATE_11K) {
            samplerate &= ~AUDIO_SAMP_RATE_11K;
            sample_rates[i] = 11025;
        } else if (samplerate & AUDIO_SAMP_RATE_16K) {
            samplerate &= ~AUDIO_SAMP_RATE_16K;
            sample_rates[i] = 16000;
        } else if (samplerate & AUDIO_SAMP_RATE_22K) {
            samplerate &= ~AUDIO_SAMP_RATE_22K;
            sample_rates[i] = 22050;
        } else if (samplerate & AUDIO_SAMP_RATE_32K) {
            samplerate &= ~AUDIO_SAMP_RATE_32K;
            sample_rates[i] = 32000;
        } else if (samplerate & AUDIO_SAMP_RATE_44K) {
            samplerate &= ~AUDIO_SAMP_RATE_44K;
            sample_rates[i] = 44100;
        } else if (samplerate & AUDIO_SAMP_RATE_48K) {
            samplerate &= ~AUDIO_SAMP_RATE_48K;
            sample_rates[i] = 48000;
        } else if (samplerate & AUDIO_SAMP_RATE_96K) {
            samplerate &= ~AUDIO_SAMP_RATE_96K;
            sample_rates[i] = 96000;
        } else if (samplerate & AUDIO_SAMP_RATE_128K) {
            samplerate &= ~AUDIO_SAMP_RATE_128K;
            sample_rates[i] = 128000;
        } else if (samplerate & AUDIO_SAMP_RATE_160K) {
            samplerate &= ~AUDIO_SAMP_RATE_160K;
            sample_rates[i] = 160000;
        } else if (samplerate & AUDIO_SAMP_RATE_172K) {
            samplerate &= ~AUDIO_SAMP_RATE_172K;
            sample_rates[i] = 172000;
        } else if (samplerate & AUDIO_SAMP_RATE_192K) {
            samplerate &= ~AUDIO_SAMP_RATE_192K;
            sample_rates[i] = 192000;
        }
    }

    return i;
}

static int ff_nuttx_flush_buffer(NuttxPriv *priv)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_s *buffer;

    if (priv->captured)
        return 0;

    buffer = (struct ap_buffer_s *)dq_peek(&priv->bufferq);
    if (!buffer || !buffer->curbyte)
        return 0;

    dq_remfirst(&priv->bufferq);

    memset(buffer->samp + buffer->curbyte, 0,
           buffer->nmaxbytes - buffer->curbyte);

    buffer->nbytes  = buffer->curbyte;
    buffer->curbyte = 0;
    desc.u.buffer   = buffer;
    return ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &desc);
}

static int ff_nuttx_get_capabilities(const char *device, int ac_type,
                                     struct audio_caps_s *caps)
{
    int ret;
    int fd;

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return AVERROR(errno);

    caps->ac_len     = sizeof(struct audio_caps_s);
    caps->ac_type    = ac_type;
    caps->ac_subtype = AUDIO_TYPE_QUERY;

    ret = ioctl(fd, AUDIOIOC_GETCAPS, caps);
    if (ret < 0)
        ret = AVERROR(errno);

    close(fd);
    return ret;
}

int ff_nuttx_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                    const char *key, int flags, bool playback)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    struct AVFormatContext *s1 = devcap->device_context;
    struct AVOptionRanges *ranges;
    struct audio_caps_s caps;
    int ret, i;

    ret = ff_nuttx_get_capabilities(s1->url,
                                    playback ? AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT,
                                    &caps);
    if (ret < 0)
        return ret;

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        goto err;

    if (!strcmp(key, "sample_fmts")) {
        ranges->nb_components = 1;
        ranges->nb_ranges = 1;
        ranges->range = av_mallocz(sizeof(AVOptionRange *));
        if (!ranges->range)
            goto err;

        ranges->range[0] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[0])
            goto err;

        ranges->range[0]->is_range  = 0;
        ranges->range[0]->value_min = AV_SAMPLE_FMT_S16;
        ranges->range[0]->value_max = AV_SAMPLE_FMT_S16;
    } else if (!strcmp(key, "channels")) {
        ranges->nb_components = 1;
        ranges->nb_ranges = 1;
        ranges->range = av_mallocz(sizeof(AVOptionRange *));
        if (!ranges->range)
            goto err;

        ranges->range[0] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[0])
            goto err;

        ranges->range[0]->is_range  = 1;
        ranges->range[0]->value_min = 1;
        ranges->range[0]->value_max = caps.ac_channels;
    } else if (!strcmp(key, "sample_rates")) {
        int sample_rates[16];

        ret = ff_nuttx_samplerate_convert(caps.ac_controls.b[0], sample_rates, 16);
        if (ret < 0)
            goto err;

        ranges->nb_components = 1;
        ranges->nb_ranges = ret;
        ranges->range = av_mallocz(sizeof(AVOptionRange *) * ret);
        if (!ranges->range)
            goto err;

        for (i = 0; i < ret; i++) {
            ranges->range[i] = av_mallocz(sizeof(AVOptionRange));
            if (!ranges->range[i])
                goto err;

            ranges->range[i]->is_range  = 0;
            ranges->range[i]->value_min = sample_rates[i];
            ranges->range[i]->value_max = sample_rates[i];
        }
    } else {
        goto err;
    }

    *ranges_ = ranges;
    return ranges->nb_components;

err:
    av_opt_freep_ranges(&ranges);
    return AVERROR(ENOMEM);
}

int ff_nuttx_get_device_list(struct AVDeviceInfoList *device_list, bool playback)
{
    int ret = 0;
    DIR *dirp;

    memset(device_list, 0, sizeof(struct AVDeviceInfoList));
    device_list->default_device = -1;

    dirp = opendir("/dev/audio");
    if (!dirp)
        return 0;

    while (1) {
        struct dirent *entryp = readdir(dirp);
        struct audio_caps_s caps;
        char str[PATH_MAX];

        if (!entryp)
            break;

        if (DIRENT_ISDIRECTORY(entryp->d_type))
            continue;

        snprintf(str, sizeof(str), "/dev/audio/%s", entryp->d_name);
        ret = ff_nuttx_get_capabilities(str, AUDIO_TYPE_QUERY, &caps);
        if (ret < 0)
            continue;

        if ((playback && (caps.ac_controls.b[0] & AUDIO_TYPE_OUTPUT)) || \
                (!playback && (caps.ac_controls.b[0] & AUDIO_TYPE_INPUT))) {
            AVDeviceInfo *device;

            device = av_mallocz(sizeof(AVDeviceInfo));
            if (!device) {
                ret = AVERROR(ENOMEM);
                goto err;
            }

            ret = av_dynarray_add_nofree(&device_list->devices,
                    &device_list->nb_devices, device);
            if (ret < 0) {
                av_free(device);
                goto err;
            }

            device->device_name = av_strdup(str);

            if (playback)
                device->device_description = av_strdup("nuttx playback device");
            else
                device->device_description = av_strdup("nuttx capture device");
        }
    }

err:
    closedir(dirp);
    return ret;
}

int ff_nuttx_init(NuttxPriv *priv, const char *device)
{
    int ret;

    struct mq_attr attr = {
        .mq_maxmsg  = 8,
        .mq_msgsize = sizeof(struct audio_msg_s),
    };

    /* open device */
    priv->fd = open(device, O_RDWR | O_CLOEXEC);
    if (priv->fd < 0)
        return AVERROR(errno);

    /* configure */
    ret = ioctl(priv->fd, AUDIOIOC_RESERVE, 0);
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    /* create message queue */
    snprintf(priv->mqname, sizeof(priv->mqname), "/tmp/%p", priv);
    priv->mq = mq_open(priv->mqname, O_RDWR | O_CREAT, 0644, &attr);
    if (priv->mq < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    ret = ioctl(priv->fd, AUDIOIOC_REGISTERMQ, priv->mq);
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    priv->volume = NAN;

    av_strlcpy(priv->devname, device, sizeof(priv->devname));

    return 0;

out:
   ff_nuttx_deinit(priv);
   return ret;
}

void ff_nuttx_deinit(NuttxPriv *priv)
{
    if (priv->fd < 0)
        return;

    ff_nuttx_close(priv, false);

    if (priv->mq >= 0) {
        ioctl(priv->fd, AUDIOIOC_UNREGISTERMQ, 0);

        mq_close(priv->mq);
        priv->mq = -1;

        mq_unlink(priv->mqname);
    }

    ioctl(priv->fd, AUDIOIOC_RELEASE, 0);

    close(priv->fd);
    priv->fd = -1;
}

int ff_nuttx_open(NuttxPriv *priv, bool playback)
{
    struct audio_caps_desc_s caps_desc = {0};
    struct audio_buf_desc_s buf_desc;
    struct ap_buffer_info_s buf_info;
    int bps, x, ret;

    if (priv->running || priv->flushing)
        return AVERROR(EAGAIN);

    bps = av_get_bits_per_sample(priv->codec);
    priv->frame_size = bps * priv->ch_layout.nb_channels / 8;
    caps_desc.caps.ac_len            = sizeof(struct audio_caps_s);
    caps_desc.caps.ac_type           = playback ?
                                       AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
    caps_desc.caps.ac_channels       = priv->ch_layout.nb_channels;
    caps_desc.caps.ac_chmap          = 0;
    caps_desc.caps.ac_controls.hw[0] = priv->sample_rate;
    caps_desc.caps.ac_controls.b[3]  = priv->sample_rate >> 16;
    caps_desc.caps.ac_controls.b[2]  = bps;
    ret = ioctl(priv->fd, AUDIOIOC_CONFIGURE, &caps_desc);
    if (ret < 0)
        return AVERROR(errno);

    if (priv->periods) {
        /* try to set BUFINFO and don't care the returns */
        if (priv->period_time)
            priv->period_bytes = priv->period_time * priv->sample_rate *
                                 priv->frame_size / 1000;
        buf_info.nbuffers    = priv->periods;
        buf_info.buffer_size = priv->period_bytes;
        ioctl(priv->fd, AUDIOIOC_SETBUFFERINFO, &buf_info);
    }

    ret = ioctl(priv->fd, AUDIOIOC_GETBUFFERINFO, &buf_info);
    if (ret >= 0) {
        priv->periods      = buf_info.nbuffers;
        priv->period_bytes = buf_info.buffer_size;
    } else {
        priv->periods      = CONFIG_AUDIO_NUM_BUFFERS;
        priv->period_bytes = CONFIG_AUDIO_BUFFER_NUMBYTES;
    }

    dq_init(&priv->bufferq);

    for (x = 0; x < priv->periods; x++) {
        struct ap_buffer_s *buffer;

        buf_desc.numbytes  = priv->period_bytes;
        buf_desc.u.pbuffer = &buffer;
        ret = ioctl(priv->fd, AUDIOIOC_ALLOCBUFFER, &buf_desc);
        if (ret < 0) {
            ret = AVERROR(errno);
            goto out;
        }

        if (playback) {
            dq_addlast(&buffer->dq_entry, &priv->bufferq);
        } else {
            buffer->nbytes    = buffer->nmaxbytes;
            buf_desc.u.buffer = buffer;
            ret = ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &buf_desc);
            if (ret < 0) {
                ret = AVERROR(errno);
                goto out;
            }
        }
    }

    ret = ioctl(priv->fd, AUDIOIOC_REGISTERMQ, priv->mq);
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    if (!playback) {
        ret = ioctl(priv->fd, AUDIOIOC_START, 0);
        if (ret < 0) {
            ret = AVERROR(errno);
            goto out;
        }

        priv->running = true;
    }

    return 0;

out:
    ff_nuttx_close(priv, false);
    return ret;
}

void ff_nuttx_close(NuttxPriv *priv, bool nonblock)
{
    struct audio_buf_desc_s buf_desc;
    int dc = dq_count(&priv->bufferq);

    if (!priv->running && !priv->flushing && dc > 0 && dc < priv->periods) {
        ioctl(priv->fd, AUDIOIOC_START, 0);
        priv->running = true;
    }

    if (priv->running) {
        ff_nuttx_flush_buffer(priv);
        ioctl(priv->fd, AUDIOIOC_STOP, 0);
        priv->running  = false;
        priv->flushing = true;
    }

    while (!dq_empty(&priv->bufferq)) {
        buf_desc.u.buffer = (struct ap_buffer_s *)dq_remfirst(&priv->bufferq);
        ioctl(priv->fd, AUDIOIOC_FREEBUFFER, &buf_desc);
    }

    while (priv->flushing) {
        ff_nuttx_poll_available(priv, nonblock);
        if (nonblock)
            break;
    }
}

int ff_nuttx_set_parameter(NuttxPriv *priv, const char *parameter)
{
    if (!priv->fd)
        return AVERROR(EINVAL);

    return ioctl(priv->fd, AUDIOIOC_SETPARAMTER, parameter);
}

int ff_nuttx_poll_available(NuttxPriv *priv, bool nonblock)
{
    struct audio_buf_desc_s buf_desc;
    int new, old = dq_count(&priv->bufferq);

    while (1) {
        struct ap_buffer_s *buffer;
        struct audio_msg_s msg;
        struct mq_attr stat;
        int ret;

        ret = mq_getattr(priv->mq, &stat);
        if (ret < 0)
            return ret;

        if (nonblock && !stat.mq_curmsgs)
            break;

        ret = mq_receive(priv->mq, (char *)&msg, sizeof(msg), NULL);
        if (ret < 0)
            return ret;

        if (msg.msg_id == AUDIO_MSG_DEQUEUE) {
            if (priv->flushing) {
                buf_desc.u.buffer = msg.u.ptr;
                ioctl(priv->fd, AUDIOIOC_FREEBUFFER, &buf_desc);
            } else {
                buffer = msg.u.ptr;
                buffer->curbyte = 0;
                dq_addlast(&buffer->dq_entry, &priv->bufferq);
            }
        } else if (msg.msg_id == AUDIO_MSG_COMPLETE) {
            ioctl(priv->fd, AUDIOIOC_RELEASE, NULL);
            priv->flushing = false;
        }

        nonblock = true;
    }

    new = dq_count(&priv->bufferq);
    if (new == priv->periods && new > old) {
        av_log(priv, AV_LOG_WARNING, "audio %s, %s !\n", priv->devname,
               priv->captured ? "capture overflow" : "playback underflow");

        if (priv->captured) {
            while (!dq_empty(&priv->bufferq)) {
                buf_desc.u.buffer = (struct ap_buffer_s *)dq_remfirst(&priv->bufferq);
                ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &buf_desc);
            }
        } else {
            ioctl(priv->fd, AUDIOIOC_PAUSE, 0);
            priv->underflow = true;
        }
    }

    return new;
}

static int ff_nuttx_peek_buffer(NuttxPriv *priv, struct ap_buffer_s **buffer)
{
    int ret;

    *buffer = (struct ap_buffer_s *)dq_peek(&priv->bufferq);
    if (*buffer)
        return 0;

    ret = ff_nuttx_poll_available(priv, priv->nonblock);
    if (ret < 0)
        return ret;

    *buffer = (struct ap_buffer_s *)dq_peek(&priv->bufferq);
    if (!*buffer)
        return AVERROR(EAGAIN);

    return 0;
}

static void ff_nuttx_drop_buffer(NuttxPriv *priv)
{
    dq_remfirst(&priv->bufferq);
}

int ff_nuttx_write_data(NuttxPriv *priv, const uint8_t *data, int size)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_s *buffer;
    int left = size;
    int ret = 0;

    while (left > 0) {
        int len;

        ret = ff_nuttx_peek_buffer(priv, &buffer);
        if (ret < 0)
            break;

        len = FFMIN(buffer->nmaxbytes - buffer->curbyte, left);
        memcpy(buffer->samp + buffer->curbyte, data, len);
        buffer->curbyte += len;

        if (buffer->curbyte == buffer->nmaxbytes) {
            ff_nuttx_drop_buffer(priv);

            buffer->curbyte = 0;
            buffer->nbytes  = buffer->nmaxbytes;
            desc.u.buffer   = buffer;
            ret = ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &desc);
            if (ret < 0) {
                ret = AVERROR(errno);
                break;
            }

            if (!priv->running && dq_count(&priv->bufferq) == 0) {
                ret = ioctl(priv->fd, AUDIOIOC_START, 0);
                if (ret < 0) {
                    ret = AVERROR(errno);
                    break;
                }

                priv->running = true;
            }

            if (priv->underflow && dq_count(&priv->bufferq) == 0) {
                ret = ioctl(priv->fd, AUDIOIOC_RESUME, 0);
                if (ret < 0) {
                    ret = AVERROR(errno);
                    break;
                }

                priv->underflow = false;
            }
        }

        data += len;
        left -= len;
    }

    return left != size ? size - left : ret;
}

int ff_nuttx_read_data(NuttxPriv *priv, uint8_t *data, int size)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_s *buffer;
    int left = size;
    int ret = 0;

    while (left > 0) {
        int len;

        ret = ff_nuttx_peek_buffer(priv, &buffer);
        if (ret < 0)
            break;

        len = FFMIN(buffer->nbytes - buffer->curbyte, left);
        memcpy(data, buffer->samp + buffer->curbyte, len);
        buffer->curbyte += len;

        if (buffer->curbyte == buffer->nbytes) {
            ff_nuttx_drop_buffer(priv);

            buffer->curbyte = 0;
            buffer->nbytes  = buffer->nmaxbytes;

            desc.u.buffer = buffer;
            ret = ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &desc);
            if (ret < 0) {
                ret = AVERROR(errno);
                break;
            }
        }

        data += len;
        left -= len;
    }

    return left != size ? size - left : ret;
}

int ff_nuttx_set_volume(struct AVFormatContext *s1, NuttxPriv *priv, double volume)
{
    struct audio_caps_desc_s caps_desc = {0};
    int ret;

    if (volume < 0 || volume > 1.0)
        return AVERROR(EINVAL);

    if (priv->volume == volume)
        return 0;

    caps_desc.caps.ac_len            = sizeof(struct audio_caps_s);
    caps_desc.caps.ac_type           = AUDIO_TYPE_FEATURE;
    caps_desc.caps.ac_format.hw      = AUDIO_FU_VOLUME;
    caps_desc.caps.ac_controls.hw[0] = volume * 1000;

    ret = ioctl(priv->fd, AUDIOIOC_CONFIGURE, &caps_desc);
    if (ret >= 0) {
        priv->volume = volume;
        ff_nuttx_notify_changed(s1, priv, true);
    }

    return ret;
}

int ff_nuttx_set_mute(struct AVFormatContext *s1, NuttxPriv *priv, bool mute)
{
    struct audio_caps_desc_s caps_desc = {0};
    int ret;

    if (priv->mute == mute)
        return 0;

    caps_desc.caps.ac_len            = sizeof(struct audio_caps_s);
    caps_desc.caps.ac_type           = AUDIO_TYPE_FEATURE;
    caps_desc.caps.ac_format.hw      = AUDIO_FU_MUTE;
    caps_desc.caps.ac_controls.hw[0] = mute;

    ret = ioctl(priv->fd, AUDIOIOC_CONFIGURE, &caps_desc);
    if (ret >= 0) {
        priv->mute = mute;
        ff_nuttx_notify_changed(s1, priv, false);
    }

    return ret;
}

int ff_nuttx_notify_changed(struct AVFormatContext *s1, NuttxPriv *priv, bool volume)
{
    if (volume)
        return avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_VOLUME_LEVEL_CHANGED,
                                                   &priv->volume, sizeof(priv->volume));
    else
        return avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_MUTE_STATE_CHANGED,
                                                   &priv->mute, sizeof(priv->mute));
}

