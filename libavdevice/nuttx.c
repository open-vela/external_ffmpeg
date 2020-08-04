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

#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

#include "nuttx.h"

av_cold static int ff_nuttx_samplerate_convert(int samplerate, int *sample_rates, int num)
{
    int i = 0;

    while (samplerate) {
        if (i >= num)
            break;

        if (samplerate & AUDIO_SAMP_RATE_8K) {
            samplerate &= ~AUDIO_SAMP_RATE_8K;
            sample_rates[i++] = 8000;
        } else if (samplerate & AUDIO_SAMP_RATE_11K) {
            samplerate &= ~AUDIO_SAMP_RATE_11K;
            sample_rates[i++] = 11025;
        } else if (samplerate & AUDIO_SAMP_RATE_16K) {
            samplerate &= ~AUDIO_SAMP_RATE_16K;
            sample_rates[i++] = 16000;
        } else if (samplerate & AUDIO_SAMP_RATE_22K) {
            samplerate &= ~AUDIO_SAMP_RATE_22K;
            sample_rates[i++] = 22050;
        } else if (samplerate & AUDIO_SAMP_RATE_32K) {
            samplerate &= ~AUDIO_SAMP_RATE_32K;
            sample_rates[i++] = 32000;
        } else if (samplerate & AUDIO_SAMP_RATE_44K) {
            samplerate &= ~AUDIO_SAMP_RATE_44K;
            sample_rates[i++] = 44100;
        } else if (samplerate & AUDIO_SAMP_RATE_48K) {
            samplerate &= ~AUDIO_SAMP_RATE_48K;
            sample_rates[i++] = 48000;
        } else if (samplerate & AUDIO_SAMP_RATE_96K) {
            samplerate &= ~AUDIO_SAMP_RATE_96K;
            sample_rates[i++] = 96000;
        } else if (samplerate & AUDIO_SAMP_RATE_128K) {
            samplerate &= ~AUDIO_SAMP_RATE_128K;
            sample_rates[i++] = 128000;
        } else if (samplerate & AUDIO_SAMP_RATE_160K) {
            samplerate &= ~AUDIO_SAMP_RATE_160K;
            sample_rates[i++] = 160000;
        } else if (samplerate & AUDIO_SAMP_RATE_172K) {
            samplerate &= ~AUDIO_SAMP_RATE_172K;
            sample_rates[i++] = 172000;
        } else if (samplerate & AUDIO_SAMP_RATE_192K) {
            samplerate &= ~AUDIO_SAMP_RATE_192K;
            sample_rates[i++] = 192000;
        }
    }

    return i;
}

av_cold static int ff_nuttx_get_capabilities(NuttxPriv *priv, const char *device,
                                             bool playback, struct audio_caps_s *rcaps)
{
    struct audio_caps_s caps;
    int ret;
    int fd;

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return AVERROR(errno);

    caps.ac_len = sizeof(caps);
    caps.ac_type = AUDIO_TYPE_QUERY;
    caps.ac_subtype = AUDIO_TYPE_QUERY;

    ret = ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&caps);
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    if (!(caps.ac_format.hw & (1 << (AUDIO_FMT_PCM - 1)))) {
        ret = AVERROR(EINVAL);
        goto out;
    }

    rcaps->ac_len = sizeof(struct audio_caps_s);
    rcaps->ac_type = playback ? AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
    rcaps->ac_subtype = AUDIO_TYPE_QUERY;

    ret = ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)rcaps);
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    ret = 0;
out:
    close(fd);
    return ret;
}

av_cold static AVOptionRange *ff_nuttx_alloc_sample_fmts(int val)
{
    AVOptionRange *range;

    range = av_mallocz(sizeof(AVOptionRange));
    if (!range)
        return NULL;

    range->is_range  = 0;
    range->value_min = val;
    range->value_max = val;

    return range;
}

av_cold int ff_nuttx_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
                                            const char *key, int flags, bool playback)
{
    struct AVDeviceCapabilitiesQuery *devcap = obj;
    struct AVFormatContext *s1 = devcap->device_context;
    NuttxPriv *priv = s1->priv_data;
    struct AVOptionRanges *ranges;
    struct audio_caps_s caps;
    int ret;
    int i;

    ret = ff_nuttx_get_capabilities(priv, s1->url, playback, &caps);
    if (ret < 0)
        return ret;

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        goto err;

    if (!strcmp(key, "sample_fmts")) {

        ranges->nb_components = 1;
        ranges->nb_ranges = 3;
        ranges->range = av_mallocz(sizeof(AVOptionRange *) * 3);
        if (!ranges->range)
            goto err;

        ranges->range[0] = ff_nuttx_alloc_sample_fmts(AV_SAMPLE_FMT_U8);
        if (!ranges->range[0])
            goto err;

        ranges->range[1] = ff_nuttx_alloc_sample_fmts(AV_SAMPLE_FMT_S16);
        if (!ranges->range[1])
            goto err;

        ranges->range[2] = ff_nuttx_alloc_sample_fmts(AV_SAMPLE_FMT_S32);
        if (!ranges->range[2])
            goto err;

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

    } else if (!strcmp(key, "channel_layouts")) {

        ranges->nb_components = 1;
        ranges->nb_ranges = caps.ac_channels;
        ranges->range = av_mallocz(sizeof(AVOptionRange *));
        if (!ranges->range)
            goto err;

        for (i = 0; i < caps.ac_channels; i++) {
            ranges->range[i] = av_mallocz(sizeof(AVOptionRange));
            if (!ranges->range[i])
                goto err;

            ranges->range[i]->is_range  = 0;
            ranges->range[i]->value_min = av_get_default_channel_layout(i+1);
            ranges->range[i]->value_max = ranges->range[i]->value_min;
        }

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
    }

    *ranges_ = ranges;

    return 0;
err:
    av_opt_freep_ranges(&ranges);
    return AVERROR(ENOMEM);
}

av_cold int ff_nuttx_get_device_list(struct AVDeviceInfoList *device_list, bool playback)
{
    DIR *dirp;
    int ret;
    int i;

    memset(device_list, 0, sizeof(struct AVDeviceInfoList));

    dirp = opendir("/dev/audio");
    if (!dirp)
        return 0;

    while (1) {
        struct dirent *entryp = readdir(dirp);
        struct audio_caps_s caps;
        char str[32];
        int fd;

        if (!entryp)
            break;

        if (DIRENT_ISDIRECTORY(entryp->d_type))
            continue;

        snprintf(str, 32, "/dev/audio/%s", entryp->d_name);
        fd = open(str, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;

        caps.ac_len = sizeof(caps);
        caps.ac_type = AUDIO_TYPE_QUERY;
        caps.ac_subtype = AUDIO_TYPE_QUERY;

        ret = ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&caps);
        close(fd);
        if (ret < 0)
            continue;

        if ((playback && (caps.ac_controls.b[0] & AUDIO_TYPE_OUTPUT)) || \
                (!playback && (caps.ac_controls.b[0] & AUDIO_TYPE_INPUT))) {
            AVDeviceInfo *device;

            device = av_mallocz(sizeof(AVDeviceInfo));
            if (!device)
                goto err;

            device->device_name = av_strdup(str);

            if (playback)
                device->device_description = av_strdup("nuttx playback device");
            else
                device->device_description = av_strdup("nuttx capture device");

            ret = av_dynarray_add_nofree(&device_list->devices,
                    &device_list->nb_devices, device);
            if (ret < 0) {
                av_free(device);
                goto err;
            }
        }
    }

    device_list->default_device = -1;
    return 0;
err:
    for (i = 0; i < device_list->nb_devices; i++) {
        av_free(device_list->devices[i]->device_name);
        av_free(device_list->devices[i]);
    }
    return AVERROR(ENOMEM);
}

static av_cold bool ff_nuttx_check_support(enum AVCodecID codec_id)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_PCM_S8:
        case AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE):
        case AV_NE(AV_CODEC_ID_PCM_S24BE, AV_CODEC_ID_PCM_S24LE):
        case AV_NE(AV_CODEC_ID_PCM_S32BE, AV_CODEC_ID_PCM_S32LE):
            return true;
        default:
            return false;
    }
}

static int ff_nuttx_set_volume_internal(NuttxPriv *priv, double volume)
{
    struct audio_caps_desc_s caps_desc = {0};
    int ret;

    caps_desc.caps.ac_len            = sizeof(struct audio_caps_s);
    caps_desc.caps.ac_type           = AUDIO_TYPE_FEATURE;
    caps_desc.caps.ac_format.hw      = AUDIO_FU_VOLUME;
    caps_desc.caps.ac_controls.hw[0] = volume * 1000;

    ret = ioctl(priv->fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps_desc);
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

static int ff_nuttx_get_volume_internal(NuttxPriv *priv, double *volume)
{
    struct audio_caps_s caps;
    int ret;

    caps.ac_len       = sizeof(struct audio_caps_s);
    caps.ac_type      = AUDIO_TYPE_FEATURE;
    caps.ac_format.hw = AUDIO_FU_VOLUME;

    ret = ioctl(priv->fd, AUDIOIOC_GETCAPS, (unsigned long)&caps);
    if (ret < 0)
        return AVERROR(errno);

    *volume = (double)caps.ac_controls.w / 1000;

    return 0;
}

static int ff_nuttx_set_mute_internal(NuttxPriv *priv, bool mute)
{
    struct audio_caps_desc_s caps_desc = {0};
    int ret;

    caps_desc.caps.ac_len            = sizeof(struct audio_caps_s);
    caps_desc.caps.ac_type           = AUDIO_TYPE_FEATURE;
    caps_desc.caps.ac_format.hw      = AUDIO_FU_MUTE;
    caps_desc.caps.ac_controls.hw[0] = mute;

    ret = ioctl(priv->fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps_desc);
    if (ret < 0)
        return AVERROR(errno);

    return 0;
}

static int ff_nuttx_get_mute_internal(NuttxPriv *priv, bool *mute)
{
    struct audio_caps_s caps;
    int ret;

    caps.ac_len       = sizeof(struct audio_caps_s);
    caps.ac_type      = AUDIO_TYPE_FEATURE;
    caps.ac_format.hw = AUDIO_FU_MUTE;

    ret = ioctl(priv->fd, AUDIOIOC_GETCAPS, (unsigned long)&caps);
    if (ret < 0)
        return AVERROR(errno);

    *mute = !!caps.ac_controls.b[0];

    return 0;
}

av_cold int ff_nuttx_open(NuttxPriv *priv, const char *device, enum AVCodecID codec_id)
{
    struct audio_caps_desc_s caps_desc = {0};
    struct audio_buf_desc_s buf_desc;
    struct ap_buffer_info_s buf_info;
    int bps, x;
    int ret;

    struct mq_attr attr = {
        .mq_maxmsg  = 8,
        .mq_msgsize = sizeof(struct audio_msg_s),
    };

    if (!ff_nuttx_check_support(codec_id))
        return AVERROR(EINVAL);

    bps = av_get_bits_per_sample(codec_id);
    priv->frame_size = bps / 8 * priv->channels;

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

    caps_desc.caps.ac_len            = sizeof(struct audio_caps_s);
    caps_desc.caps.ac_type           = priv->playback ?
                                       AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
    caps_desc.caps.ac_channels       = priv->channels;
    caps_desc.caps.ac_chmap          = priv->channel_layout ?
                                       priv->channel_layout :
                                       ~(0xff << priv->channels);
    caps_desc.caps.ac_controls.hw[0] = priv->sample_rate;
    caps_desc.caps.ac_controls.b[3]  = priv->sample_rate >> 16;
    caps_desc.caps.ac_controls.b[2]  = bps;
    ret = ioctl(priv->fd, AUDIOIOC_CONFIGURE, (unsigned long)&caps_desc);
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    /* create message queue */
    snprintf(priv->mqname, sizeof(priv->mqname), "/tmp/%0lx",
            (unsigned long)((uintptr_t)priv));
    priv->mq = mq_open(priv->mqname, O_RDWR | O_CREAT, 0644, &attr);
    if (priv->mq < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    ret = ioctl(priv->fd, AUDIOIOC_REGISTERMQ, (unsigned long)priv->mq);
    if (ret < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    if (priv->periods && priv->period_bytes) {
        /* try to set BUFINFO and don't care the returns */
        if (priv->period_time)
            priv->period_bytes = priv->period_time * priv->sample_rate *
                                 priv->channels * bps / 8000;
        buf_info.nbuffers    = priv->periods;
        buf_info.buffer_size = priv->period_bytes;
        ioctl(priv->fd, AUDIOIOC_SETBUFFERINFO, (unsigned long)&buf_info);
    }

    ret = ioctl(priv->fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info);
    if (ret >= 0) {
        priv->periods      = buf_info.nbuffers;
        priv->period_bytes = buf_info.buffer_size;
    } else {
        priv->periods      = CONFIG_AUDIO_NUM_BUFFERS;
        priv->period_bytes = CONFIG_AUDIO_BUFFER_NUMBYTES;
    }

    /* create audio abuffer */
    priv->abuffer = av_mallocz(priv->periods * sizeof(struct ap_buffer_s *));
    if (!priv->abuffer) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    for (x = 0; x < priv->periods; x++) {
        buf_desc.numbytes  = priv->period_bytes;
        buf_desc.u.pbuffer = &priv->abuffer[x];
        ret = ioctl(priv->fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc);
        if (ret < 0) {
            ret = AVERROR(errno);
            goto out;
        }
    }

    /* enqueuebuffer if capture */
    if (!priv->playback) {
        for (x = 0; x < priv->periods; x++) {
            buf_desc.u.buffer = priv->abuffer[x];
            ret = ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
            if (ret < 0)
                goto out;
        }
    } else {
        priv->free_abuffer = priv->periods;
    }

    if (priv->volume)
        ff_nuttx_set_volume_internal(priv, priv->volume);
    else
        ff_nuttx_get_volume_internal(priv, &priv->volume);

    if (priv->mute)
        ff_nuttx_set_mute_internal(priv, priv->mute);
    else
        ff_nuttx_get_mute_internal(priv, &priv->mute);

    /* start audio */
    ret = ioctl(priv->fd, AUDIOIOC_START, 0);
    if (ret < 0)
        ret = AVERROR(errno);

out:
    if (ret < 0)
        ff_nuttx_close(priv);
    return ret;
}

av_cold int ff_nuttx_close(NuttxPriv *priv)
{
    struct audio_msg_s msg;
    int i;

    if (priv->fd < 0)
        return 0;

    while (1) {
        struct mq_attr stat;
        int ret;

        ret = mq_getattr(priv->mq, &stat);
        if (ret < 0)
            return ret;

        if (!stat.mq_curmsgs)
            break;

        mq_receive(priv->mq, (char *)&msg, sizeof(msg), NULL);
    }

    ioctl(priv->fd, AUDIOIOC_STOP, 0);

    while (1) {
        mq_receive(priv->mq, (char *)&msg, sizeof(msg), NULL);
        if (msg.msg_id == AUDIO_MSG_COMPLETE)
            break;
    }

    if (priv->mq) {

        ioctl(priv->fd, AUDIOIOC_UNREGISTERMQ, NULL);

        mq_close(priv->mq);
        priv->mq = NULL;

        mq_unlink(priv->mqname);
    }

    ioctl(priv->fd, AUDIOIOC_RELEASE, 0);

    if (priv->abuffer) {
        for (i = 0; i < priv->periods; i++) {
            struct audio_buf_desc_s buf_desc;

            buf_desc.u.buffer = priv->abuffer[i];
            ioctl(priv->fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
        }

        free(priv->abuffer);
        priv->abuffer = NULL;
    }

    priv->abuffer_cur = NULL;
    priv->buffer_pos  = 0;

    close(priv->fd);
    priv->fd = -1;

    return 0;
}

static int ff_nuttx_get_buffer(NuttxPriv *priv, struct ap_buffer_s **abuffer)
{
    struct audio_msg_s msg;
    struct mq_attr stat;
    ssize_t size;
    int ret;

    *abuffer = NULL;

    if (priv->free_abuffer) {
        *abuffer = priv->abuffer[--priv->free_abuffer];
        return 0;
    }

    if (priv->nonblock) {
        ret = mq_getattr(priv->mq, &stat);
        if (ret < 0)
            return ret;

        if (!stat.mq_curmsgs)
            return AVERROR(EAGAIN);
    }

    while (1) {
        size = mq_receive(priv->mq, (char *)&msg, sizeof(msg), NULL);
        if (size != sizeof(msg))
            return AVERROR(EIO);

        if (msg.msg_id == AUDIO_MSG_DEQUEUE) {
            *abuffer = msg.u.ptr;
            break;
        }
    }

    return 0;
}

int ff_nuttx_write_data(NuttxPriv *priv, uint8_t *buf, int size)
{
    struct ap_buffer_s *abuffer = priv->abuffer_cur;
    struct audio_buf_desc_s desc;
    int ret;

    while (size > 0) {
        int len;

        if (priv->buffer_pos == 0) {
            ret = ff_nuttx_get_buffer(priv, &abuffer);
            if (ret < 0)
                return ret;

            priv->abuffer_cur = abuffer;
        }

        len = FFMIN(priv->period_bytes - priv->buffer_pos, size);

        memcpy(abuffer->samp + priv->buffer_pos, buf, len);

        priv->buffer_pos += len;
        if (priv->buffer_pos == priv->period_bytes) {

            priv->buffer_pos = 0;
            abuffer->nbytes  = priv->period_bytes;
            desc.u.buffer    = abuffer;

            ret = ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc);
            if (ret < 0) {
                ret = AVERROR(errno);
                break;
            }
        }

        buf  += len;
        size -= len;
    }

    return ret;
}

int ff_nuttx_read_data(NuttxPriv *priv, uint8_t *buf, int *size)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_s *abuffer;
    int ret;

    if (*size < priv->period_bytes)
        return AVERROR(EIO);

    ret = ff_nuttx_get_buffer(priv, &abuffer);
    if (ret < 0)
        return ret;

    *size = FFMIN(*size, abuffer->nbytes);
    memcpy(buf, abuffer->samp, *size);

    abuffer->nbytes = abuffer->nmaxbytes;
    desc.u.buffer = abuffer;
    ret = ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&desc);
    if (ret < 0)
        ret = AVERROR(errno);

    return ret;
}

int ff_nuttx_set_volume(struct AVFormatContext *s1, NuttxPriv *priv, double volume)
{
    int ret = 0;

    if (volume < 0 || volume > 1.0)
        return AVERROR(EINVAL);

    if (priv->volume == volume)
        return 0;

    if (priv->fd)
        ret = ff_nuttx_set_volume_internal(priv, volume);

    if (ret >= 0) {
        priv->volume = volume;
        ff_nuttx_notify_changed(s1, priv, true);
    }

    return ret;
}

int ff_nuttx_set_mute(struct AVFormatContext *s1, NuttxPriv *priv, bool mute)
{
    int ret = 0;

    if (priv->mute == mute)
        return 0;

    if (priv->fd)
        ret = ff_nuttx_set_mute_internal(priv, mute);

    if (ret >= 0) {
        priv->mute = mute;
        ff_nuttx_notify_changed(s1, priv, false);
    }

    return ret;
}

int ff_nuttx_notify_changed(struct AVFormatContext *s1, NuttxPriv *priv, bool volume)
{
    int ret;

    if (volume)
        ret = avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_VOLUME_LEVEL_CHANGED,
                                                  &priv->volume, sizeof(priv->volume));
    else
        ret = avdevice_dev_to_app_control_message(s1, AV_DEV_TO_APP_MUTE_STATE_CHANGED,
                                                  &priv->mute, sizeof(priv->mute));

    return ret;
}

