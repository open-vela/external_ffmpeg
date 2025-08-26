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
#include "libavutil/mem.h"

#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

#include "nuttx.h"

#undef ff_nuttx_close
#undef ff_nuttx_poll_available

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
        } else if (samplerate & AUDIO_SAMP_RATE_12K) {
            samplerate &= ~AUDIO_SAMP_RATE_12K;
            sample_rates[i] = 12000;
        } else if (samplerate & AUDIO_SAMP_RATE_16K) {
            samplerate &= ~AUDIO_SAMP_RATE_16K;
            sample_rates[i] = 16000;
        } else if (samplerate & AUDIO_SAMP_RATE_22K) {
            samplerate &= ~AUDIO_SAMP_RATE_22K;
            sample_rates[i] = 22050;
        } else if (samplerate & AUDIO_SAMP_RATE_24K) {
            samplerate &= ~AUDIO_SAMP_RATE_24K;
            sample_rates[i] = 24000;
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

static int ff_nuttx_avcodec_to_fmt(int codec_id)
{
    switch (codec_id) {
        case AV_CODEC_ID_MP3:
            return AUDIO_FMT_MP3;
        case AV_CODEC_ID_AC3:
            return AUDIO_FMT_AC3;
        case AV_CODEC_ID_WMAV2:
            return AUDIO_FMT_WMA;
        case AV_CODEC_ID_DTS:
            return AUDIO_FMT_DTS;
        case AV_CODEC_ID_VORBIS:
            return AUDIO_FMT_OGG_VORBIS;
        case AV_CODEC_ID_FLAC:
            return AUDIO_FMT_FLAC;
        case AV_CODEC_ID_AMR_NB:
            return AUDIO_FMT_AMR;
        case AV_CODEC_ID_AMR_WB:
            return AUDIO_FMT_AMRWB;
        case AV_CODEC_ID_OPUS:
            return AUDIO_FMT_OPUS;
        case AV_CODEC_ID_AAC:
            return AUDIO_FMT_AAC;
    }

    return AUDIO_FMT_PCM;
}

static int ff_nuttx_subfmt_to_avcodec(int subfmt)
{
    switch (subfmt) {
        case AUDIO_SUBFMT_PCM_U8:     return AV_CODEC_ID_PCM_U8;
        case AUDIO_SUBFMT_PCM_S8:     return AV_CODEC_ID_PCM_S8;
        case AUDIO_SUBFMT_PCM_U16_LE: return AV_CODEC_ID_PCM_U16LE;
        case AUDIO_SUBFMT_PCM_U16_BE: return AV_CODEC_ID_PCM_U16BE;
        case AUDIO_SUBFMT_PCM_S16_LE: return AV_CODEC_ID_PCM_S16LE;
        case AUDIO_SUBFMT_PCM_S16_BE: return AV_CODEC_ID_PCM_S16BE;
        case AUDIO_SUBFMT_PCM_U32_LE: return AV_CODEC_ID_PCM_U32LE;
        case AUDIO_SUBFMT_PCM_U32_BE: return AV_CODEC_ID_PCM_U32BE;
        case AUDIO_SUBFMT_PCM_S32_LE: return AV_CODEC_ID_PCM_S32LE;
        case AUDIO_SUBFMT_PCM_S32_BE: return AV_CODEC_ID_PCM_S32BE;
        case AUDIO_SUBFMT_PCM_MU_LAW: return AV_CODEC_ID_PCM_MULAW;
        case AUDIO_SUBFMT_PCM_A_LAW:  return AV_CODEC_ID_PCM_ALAW;
        case AUDIO_SUBFMT_PCM_MP1:    return AV_CODEC_ID_MP1;
        case AUDIO_SUBFMT_PCM_MP2:    return AV_CODEC_ID_MP2;
        case AUDIO_SUBFMT_PCM_MP3:    return AV_CODEC_ID_MP3;
    }

    return AV_CODEC_ID_FIRST_AUDIO;
}

static int ff_nuttx_subfmt_to_smpfmt(int subfmt)
{
    switch (subfmt) {
        case AUDIO_SUBFMT_PCM_U8:     return AV_SAMPLE_FMT_U8;
        case AUDIO_SUBFMT_PCM_S16_LE:
        case AUDIO_SUBFMT_PCM_S16_BE: return AV_SAMPLE_FMT_S16;
        case AUDIO_SUBFMT_PCM_S32_LE:
        case AUDIO_SUBFMT_PCM_S32_BE: return AV_SAMPLE_FMT_S32;
    }

    return AV_SAMPLE_FMT_NONE;
}

static int ff_nuttx_ioctl(int fd, int cmd, unsigned long arg)
{
    int ret;

    ret = ioctl(fd, cmd, arg);
    if (ret < 0)
        ret = AVERROR(errno);

    return ret;
}
#define ff_nuttx_ioctl(fd, cmd, arg) ff_nuttx_ioctl(fd, cmd, (unsigned long)(arg))

static int ff_nuttx_enqueue_buffer(NuttxPriv *priv, struct ap_buffer_s *buffer, bool eos)
{
    struct audio_buf_desc_s desc;

    if (buffer->curbyte != buffer->nmaxbytes)
        memset(buffer->samp + buffer->curbyte, 0,
               buffer->nmaxbytes - buffer->curbyte);

    if (eos) {
        buffer->flags |= AUDIO_APB_FINAL;
        av_log(NULL, AV_LOG_INFO, "[%s][%s] apb final.\n", __func__, priv->devname);
    } else {
        buffer->flags &= ~AUDIO_APB_FINAL;
    }

    buffer->nbytes  = buffer->curbyte;
    buffer->curbyte = 0;
    desc.u.buffer   = buffer;
    return ff_nuttx_ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &desc);
}

static int ff_nuttx_get_capabilities(const char *device, int ac_type, int ac_subtype,
                                     struct audio_caps_s *caps)
{
    int ret;
    int fd;

    fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return AVERROR(errno);

    caps->ac_len     = sizeof(struct audio_caps_s);
    caps->ac_type    = ac_type;
    caps->ac_subtype = ac_subtype;

    ret = ff_nuttx_ioctl(fd, AUDIOIOC_GETCAPS, caps);
    close(fd);
    return ret;
}

static int ff_nuttx_set_ranges(struct AVOptionRanges *ranges, int nb_ranges, int is_range,
                               int min_v[], int max_v[])
{
    ranges->nb_components = 1;
    ranges->nb_ranges = nb_ranges;

    ranges->range = av_mallocz(nb_ranges * sizeof(AVOptionRange *));
    if (!ranges->range)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_ranges; i++) {
        ranges->range[i] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[i])
            return AVERROR(ENOMEM);

        ranges->range[i]->is_range  = is_range;
        ranges->range[i]->value_min = min_v[i];
        ranges->range[i]->value_max = is_range ? max_v[i] : min_v[i];
    }

    return 0;
}

static int ff_nuttx_capbility_query_smpfmts(const char *device, int format, int values[])
{
    struct audio_caps_s smpfmts;
    int ret, x;

    if (((format & (1 << (AUDIO_FMT_PCM - 1))) == 0))
        return AVERROR(EPERM);

    ret = ff_nuttx_get_capabilities(device, AUDIO_TYPE_QUERY, AUDIO_FMT_PCM, &smpfmts);
    if (ret < 0)
        return ret;

    for (x = 0; x < sizeof(smpfmts.ac_controls.b); x++) {
        if (smpfmts.ac_controls.b[x] == AUDIO_SUBFMT_END)
            break;

        ret = ff_nuttx_subfmt_to_smpfmt(smpfmts.ac_controls.b[x]);
        if (ret >= 0)
            values[x] = ret;
    }

    return x == 0 ? AVERROR(EPERM) : x;
}

static int ff_nuttx_fmt_to_avcodec(int *codec_id, int *formats)
{
    int codec  = AV_CODEC_ID_NONE;
    int format = AUDIO_FMT_UNDEF;

    if (*formats & (1 << (AUDIO_FMT_PCM - 1))) {
        codec     = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE);
        format    = AUDIO_FMT_PCM;
        *formats &= ~(1 << (AUDIO_FMT_PCM - 1));
    } else if (*formats & (1 << (AUDIO_FMT_MP3 - 1))) {
        codec     = AV_CODEC_ID_MP3;
        format    = AUDIO_FMT_MP3;
        *formats &= ~(1 << (AUDIO_FMT_MP3 - 1));
    } else if (*formats & (1 << (AUDIO_FMT_AC3 - 1))) {
        codec     = AV_CODEC_ID_AC3;
        format    = AUDIO_FMT_AC3;
        *formats &= ~(1 << (AUDIO_FMT_AC3 - 1));
    } else if (*formats & (1 << (AUDIO_FMT_WMA - 1))) {
        codec     = AV_CODEC_ID_WMAV2;
        format    = AUDIO_FMT_WMA;
        *formats &= ~(1 << (AUDIO_FMT_WMA - 1));
    } else if (*formats & (1 << (AUDIO_FMT_DTS - 1))) {
        codec     = AV_CODEC_ID_DTS;
        format    = AUDIO_FMT_WMA;
        *formats &= ~(1 << (AUDIO_FMT_DTS - 1));
    } else if (*formats & (1 << (AUDIO_FMT_OGG_VORBIS - 1))) {
        codec     = AV_CODEC_ID_VORBIS;
        format    = AUDIO_FMT_OGG_VORBIS;
        *formats &= ~(1 << (AUDIO_FMT_OGG_VORBIS - 1));
    } else if (*formats & (1 << (AUDIO_FMT_FLAC - 1))) {
        codec     = AV_CODEC_ID_FLAC;
        format    = AUDIO_FMT_FLAC;
        *formats &= ~(1 << (AUDIO_FMT_FLAC - 1));
    } else if (*formats & (1 << (AUDIO_FMT_AMR - 1))) {
        codec     = AV_CODEC_ID_AMR_NB;
        format    = AUDIO_FMT_AMR;
        *formats &= ~(1 << (AUDIO_FMT_AMR - 1));
    } else if (*formats & (1 << (AUDIO_FMT_AMRWB - 1))) {
        codec     = AV_CODEC_ID_AMR_WB;
        format    = AUDIO_FMT_AMRWB;
        *formats &= ~(1 << (AUDIO_FMT_AMRWB - 1));
    } else if (*formats & (1 << (AUDIO_FMT_OTHER - 1))) {
        format    = AUDIO_FMT_OTHER;
        *formats &= ~(1 << (AUDIO_FMT_OTHER - 1));
    } else if (*formats & (1 << (AUDIO_FMT_AAC - 1))) {
        codec     = AV_CODEC_ID_AAC;
        format    = AUDIO_FMT_AAC;
        *formats &= ~(1 << (AUDIO_FMT_AAC - 1));
    } else if (*formats & (1 << (AUDIO_FMT_OPUS - 1))) {
        codec     = AV_CODEC_ID_OPUS;
        format    = AUDIO_FMT_OPUS;
        *formats &= ~(1 << (AUDIO_FMT_OPUS - 1));
    }

    *codec_id = codec;

    return format;
}

static int ff_nuttx_capbility_query_codecs(const char *device,
                                           int format, int codecs[], int num)
{
    int ac_subtype = AUDIO_FMT_UNDEF;
    int codec = AV_CODEC_ID_NONE;
    struct audio_caps_s caps;
    int i, nb_codecs = 0;

    while (nb_codecs < num && format) {
        ac_subtype = ff_nuttx_fmt_to_avcodec(&codec, &format);
        if (ff_nuttx_get_capabilities(device, AUDIO_TYPE_QUERY, ac_subtype, &caps) < 0)
            continue;

        if (ac_subtype == AUDIO_FMT_OTHER) {
            nb_codecs += ff_nuttx_capbility_query_codecs(device, caps.ac_controls.w, &codecs[nb_codecs], num - nb_codecs);
            continue;
        }

        for (i = 0; i < sizeof(caps.ac_controls.b) && nb_codecs < num; i++) {
            if (caps.ac_controls.b[i] == AUDIO_SUBFMT_END) {
                if (i == 0)
                    codecs[nb_codecs++] = codec;
                break;
            }

            codecs[nb_codecs++] = ff_nuttx_subfmt_to_avcodec(caps.ac_controls.b[i]);
        }
    }

    return nb_codecs;
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

static int ff_nuttx_drain_buffer(NuttxPriv *priv, bool eos)
{
    struct ap_buffer_s *buffer;
    int ret;

    if (!priv->playback)
        return 0;

    ret = ff_nuttx_peek_buffer(priv, &buffer);
    if (ret < 0)
        return ret;

    if (!eos && !buffer->curbyte)
        return 0;

    dq_remfirst(&priv->bufferq);
    return ff_nuttx_enqueue_buffer(priv, buffer, eos);
}

int ff_nuttx_capbility_query_ranges(struct AVOptionRanges **ranges_, const char *device,
                                    const char *key, int flags, bool playback)
{
    struct audio_caps_s formats = { 0 };
    struct audio_caps_s others = { 0 };
    int ac_type = AUDIO_TYPE_QUERY;
    struct AVOptionRanges *ranges;
    int values0[64], values1[64];
    int nb_ranges, is_range = 0;
    int ret;

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        return AVERROR(ENOMEM);

    if (!strcmp(key, "sample_fmts") || !strcmp(key, "codecs")) {
        ret = ff_nuttx_get_capabilities(device, ac_type, AUDIO_TYPE_QUERY, &formats);
        if (ret < 0)
            goto err;

        if (!strcmp(key, "sample_fmts")) {
            ret = ff_nuttx_capbility_query_smpfmts(device, formats.ac_format.hw, values0);
            if (ret < 0)
                goto err;
        } else {
            ret = ff_nuttx_capbility_query_codecs(device, formats.ac_format.hw, values0, 64);
            if (ret < 0)
                goto err;
        }

        nb_ranges = ret;
    } else if (!strcmp(key, "channels") || !strcmp(key, "sample_rates")) {
        ac_type = playback ? AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
        ret = ff_nuttx_get_capabilities(device, ac_type, AUDIO_TYPE_QUERY, &others);
        if (ret < 0)
            goto err;

        if (!strcmp(key, "channels")) {
            if ((others.ac_channels & 0xf0) == 0) {
                values0[0] = 1;
                values1[0] = others.ac_channels;
            } else {
                values0[0] = others.ac_channels >> 4;
                values1[0] = others.ac_channels & 0x0f;
            }

            nb_ranges = 1;
            is_range  = (values0[0] != values1[0]);
        } else {
            ret = ff_nuttx_samplerate_convert(others.ac_controls.hw[0], values0, 64);
            if (ret < 0)
                goto err;

            nb_ranges = ret;
        }
    } else {
        ret = -EINVAL;
        goto err;
    }

    ret = ff_nuttx_set_ranges(ranges, nb_ranges, is_range, values0, values1);
    if (ret < 0)
        goto err;

    *ranges_ = ranges;
    return ranges->nb_components;

err:
    av_opt_freep_ranges(&ranges);
    return ret;
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
        ret = ff_nuttx_get_capabilities(str, AUDIO_TYPE_QUERY, AUDIO_TYPE_QUERY, &caps);
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

int ff_nuttx_init(NuttxPriv *priv, const char *device, bool playback)
{
    int ret;

    struct mq_attr attr = {
        .mq_maxmsg  = 16,
        .mq_msgsize = sizeof(struct audio_msg_s),
    };

    /* open device */
    priv->fd = open(device, O_RDWR | O_CLOEXEC);
    if (priv->fd < 0)
        return AVERROR(errno);

    /* configure */
    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_RESERVE, 0);
    if (ret < 0)
        goto out;

    /* create message queue */
    snprintf(priv->mqname, sizeof(priv->mqname), "/tmp/%p", priv);
    priv->mq = mq_open(priv->mqname, O_RDWR | O_CREAT | O_CLOEXEC, 0644,
                       &attr);
    if (priv->mq < 0) {
        ret = AVERROR(errno);
        goto out;
    }

    priv->playback = playback;
    priv->volume = NAN;
    priv->mute = false;

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
        ff_nuttx_ioctl(priv->fd, AUDIOIOC_UNREGISTERMQ, 0);

        mq_close(priv->mq);
        priv->mq = -1;

        mq_unlink(priv->mqname);
    }

    ff_nuttx_ioctl(priv->fd, AUDIOIOC_RELEASE, 0);

    close(priv->fd);
    priv->fd = -1;
}

int ff_nuttx_open(NuttxPriv *priv)
{
    struct audio_caps_desc_s caps_desc = {0};
    struct audio_buf_desc_s buf_desc;
    struct ap_buffer_info_s buf_info;
    int bps, x, ret;

    if (priv->running || priv->draining)
        return AVERROR(EAGAIN);

    bps = av_get_bits_per_sample(priv->codec);
    if (bps == 0)
        bps = av_get_bytes_per_sample(priv->format) * 8;

    priv->sample_bytes               = bps * priv->ch_layout.nb_channels / 8;
    caps_desc.caps.ac_len            = sizeof(struct audio_caps_s);
    caps_desc.caps.ac_type           = priv->playback ?
                                       AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
    caps_desc.caps.ac_channels       = priv->ch_layout.nb_channels;
    caps_desc.caps.ac_chmap          = 0;
    caps_desc.caps.ac_controls.hw[0] = priv->sample_rate;
    caps_desc.caps.ac_controls.b[3]  = priv->sample_rate >> 16;
    caps_desc.caps.ac_controls.b[2]  = bps;
    caps_desc.caps.ac_subtype        = ff_nuttx_avcodec_to_fmt(priv->codec);

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_CONFIGURE, &caps_desc);
    av_log(NULL, AV_LOG_INFO, "[%s][%s] configure, sr:%"PRIu32" ch:%d ret:%d\n",
        __func__, priv->devname, priv->sample_rate, priv->ch_layout.nb_channels, ret);
    if (ret < 0)
        return ret;

    if (priv->periods) {
        /* try to set BUFINFO and don't care the returns */
        if (priv->period_time)
            priv->period_bytes = priv->period_time * priv->sample_rate *
                                 priv->sample_bytes / 1000;
        buf_info.nbuffers    = priv->periods;
        buf_info.buffer_size = priv->period_bytes;
        av_log(NULL, AV_LOG_INFO, "[%s][%s] set buffer info, n:%d size:%d\n",
            __func__, priv->devname, buf_info.nbuffers, buf_info.buffer_size);
        ff_nuttx_ioctl(priv->fd, AUDIOIOC_SETBUFFERINFO, &buf_info);
    }

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_GETBUFFERINFO, &buf_info);
    av_log(NULL, AV_LOG_INFO, "[%s][%s] get buffer info, n:%d size:%d ret:%d\n",
            __func__, priv->devname, buf_info.nbuffers, buf_info.buffer_size, ret);
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
        ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_ALLOCBUFFER, &buf_desc);
        if (ret < 0)
            goto out;

        if (priv->playback) {
            dq_addlast(&buffer->dq_entry, &priv->bufferq);
        } else {
            buffer->nbytes    = buffer->nmaxbytes;
            buf_desc.u.buffer = buffer;
            ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &buf_desc);
            if (ret < 0)
                goto out;
        }
    }

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_REGISTERMQ, priv->mq);
    if (ret < 0)
        goto out;

    if (!priv->playback) {
        ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_START, 0);
        av_log(NULL, AV_LOG_INFO, "[%s][%s] start ret:%d\n", __func__, priv->devname, ret);
        if (ret < 0)
            goto out;

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

    if (!priv->running && !priv->draining && dc > 0 && dc <= priv->periods) {
        av_log(NULL, AV_LOG_INFO, "[%s][%s] start\n", __func__, priv->devname);
        ff_nuttx_ioctl(priv->fd, AUDIOIOC_START, 0);
        priv->running = true;
    }

    if (priv->running) {
        ff_nuttx_drain_buffer(priv, false);
        av_log(NULL, AV_LOG_INFO, "[%s][%s] stop\n", __func__, priv->devname);
        ff_nuttx_ioctl(priv->fd, AUDIOIOC_STOP, 0);
        priv->running  = false;
        priv->draining = true;
    }

    while (!dq_empty(&priv->bufferq)) {
        buf_desc.u.buffer = (struct ap_buffer_s *)dq_remfirst(&priv->bufferq);
        ff_nuttx_ioctl(priv->fd, AUDIOIOC_FREEBUFFER, &buf_desc);
    }

    while (priv->draining) {
        ff_nuttx_poll_available(priv, nonblock);
        if (nonblock)
            break;
    }

    priv->paused = false;
}

int ff_nuttx_set_parameter(NuttxPriv *priv, const char *parameter)
{
    if (!priv->fd)
        return AVERROR(EINVAL);

    return ff_nuttx_ioctl(priv->fd, AUDIOIOC_SETPARAMTER, parameter);
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
            return AVERROR(errno);

        if (nonblock && !stat.mq_curmsgs)
            break;

        ret = mq_receive(priv->mq, (char *)&msg, sizeof(msg), NULL);
        if (ret < 0)
            return AVERROR(errno);

        if (msg.msg_id == AUDIO_MSG_DEQUEUE) {
            if (!priv->running && priv->draining) {
                buf_desc.u.buffer = msg.u.ptr;
                ff_nuttx_ioctl(priv->fd, AUDIOIOC_FREEBUFFER, &buf_desc);
            } else {
                buffer = msg.u.ptr;
                buffer->curbyte = 0;
                dq_addlast(&buffer->dq_entry, &priv->bufferq);
            }
        } else if (msg.msg_id == AUDIO_MSG_COMPLETE) {
            av_log(priv, AV_LOG_INFO, "[%s][%s] complete\n", __func__, priv->devname);
            priv->draining = false;
            if (priv->mq >= 0)
                ff_nuttx_ioctl(priv->fd, AUDIOIOC_UNREGISTERMQ, 0);
            if (!priv->running)
                ff_nuttx_ioctl(priv->fd, AUDIOIOC_RELEASE, NULL);
            else
                return AVERROR_EXIT;
        } else if (msg.msg_id == AUDIO_MSG_UNDERRUN) {
            av_log(priv, AV_LOG_WARNING, "[%s][%s] underflow! pause.\n", __func__, priv->devname);
            ff_nuttx_ioctl(priv->fd, AUDIOIOC_PAUSE, 0);
            priv->paused = true;
        } else if (msg.msg_id == AUDIO_MSG_IOERR) {
            av_log(priv, AV_LOG_ERROR, "[%s][%s]io error occur\n", __func__, priv->devname);
            priv->ioerr = true;
        }

        nonblock = true;
    }

    new = dq_count(&priv->bufferq);
    if (priv->periods > 1 && new == priv->periods && new > old && !priv->paused && priv->running) {
        if (priv->playback) {
            /* Try to send the buffer containing the remaining data to the driver */
            ff_nuttx_drain_buffer(priv, false);
            old = new;
            new = dq_count(&priv->bufferq);
            if (new >= old) {
                /* No buffer is sent to the driver,execute pause */
                av_log(priv, AV_LOG_WARNING, "[%s][%s] playback underflow! pause.\n",
                       __func__, priv->devname);
                ff_nuttx_ioctl(priv->fd, AUDIOIOC_PAUSE, 0);
                priv->paused = true;
            } else {
                /* A buffer is sent to the driver,pause will be done next time */
                av_log(priv, AV_LOG_INFO, "[%s][%s] enqueue remaining data\n",
                       __func__, priv->devname);
            }
        } else {
            av_log(priv, AV_LOG_WARNING, "[%s][%s] capture overflow!\n",
                   __func__, priv->devname);
            while (!dq_empty(&priv->bufferq)) {
                buf_desc.u.buffer = (struct ap_buffer_s *)dq_remfirst(&priv->bufferq);
                ff_nuttx_ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &buf_desc);
            }
        }
    }

    return new;
}

long ff_nuttx_get_latency(NuttxPriv *priv)
{
    struct ap_buffer_s *buffer;
    struct dq_entry_s *cur;
    long latency = 0;
    int count = 0;
    int ret;

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_GETLATENCY, &latency);
    if (ret < 0)
        return ret;

    for (cur = dq_peek(&priv->bufferq); cur; cur = dq_next(cur)) {
        buffer = (struct ap_buffer_s *)cur;
        count += buffer->curbyte;
    }

    return latency + count / priv->sample_bytes;
}

int ff_nuttx_write_data(NuttxPriv *priv, const uint8_t *data, int size)
{
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
            dq_remfirst(&priv->bufferq);

            ret = ff_nuttx_enqueue_buffer(priv, buffer, false);
            if (ret < 0)
                break;

            if (!priv->running && dq_count(&priv->bufferq) == 0) {
                ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_START, 0);
                av_log(NULL, AV_LOG_INFO, "[%s][%s] start ret:%d\n", __func__, priv->devname, ret);
                if (ret < 0)
                    break;

                priv->running = true;
            }

            if (priv->paused && dq_count(&priv->bufferq) == 0) {
                ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_RESUME, 0);
                av_log(NULL, AV_LOG_INFO, "[%s][%s] resume ret:%d\n", __func__, priv->devname, ret);
                if (ret < 0)
                    break;

                priv->paused = false;
            }
        }

        data += len;
        left -= len;
    }

    /* eos is on, consume remaining data. */
    if (size == 0) {
        ret = ff_nuttx_drain_buffer(priv, true);

        if (!priv->running) {
            ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_START, 0);
            if (ret < 0)
                return ret;

            priv->running = true;
        } else if (priv->paused) {
            ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_RESUME, 0);
            if (ret < 0)
                return ret;

            priv->paused = false;
        }
    }

    return left != size ? size - left : ret;
}

int ff_nuttx_read_data(NuttxPriv *priv, uint8_t *data, int size, uint32_t *samples)
{
    struct audio_buf_desc_s desc;
    struct ap_buffer_s *buffer;
    int left = size;
    int ret = 0;

    *samples = 0;

    if (priv->ioerr) {
        priv->ioerr = false;
        return AVERROR_EOF;
    }

    while (left > 0) {
        int len;

        ret = ff_nuttx_peek_buffer(priv, &buffer);
        if (ret < 0)
            break;

        len = FFMIN(buffer->nbytes - buffer->curbyte, left);
        memcpy(data, buffer->samp + buffer->curbyte, len);
        if (priv->mute)
            memset(data, 0x00, len);

        data += len;
        left -= len;

        buffer->curbyte += len;
        if (buffer->curbyte == buffer->nbytes) {
            uint32_t nsamples = buffer->nsamples;

            dq_remfirst(&priv->bufferq);

            buffer->curbyte = 0;
            buffer->nbytes  = buffer->nmaxbytes;

            desc.u.buffer = buffer;
            ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_ENQUEUEBUFFER, &desc);
            if (ret < 0)
                break;
        }
    }

    return left != size ? size - left : ret;
}

int ff_nuttx_pause(NuttxPriv *priv)
{
    int ret;

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_PAUSE, 0);
    av_log(NULL, AV_LOG_INFO, "[%s][%s] pause ret:%d\n", __func__, priv->devname, ret);
    if (ret >= 0)
        priv->paused = true;

    return ret;
}

int ff_nuttx_resume(NuttxPriv *priv)
{
    int ret;

    if ((!priv->running) || (dq_count(&priv->bufferq) == priv->periods))
        return 0;

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_RESUME, 0);
    av_log(NULL, AV_LOG_INFO, "[%s][%s] resume ret:%d\n", __func__, priv->devname, ret);
    if (ret >= 0)
        priv->paused = false;

    return ret;
}

int ff_nuttx_flush(NuttxPriv *priv)
{
    if (!priv->running)
        return 0;

    return ff_nuttx_ioctl(priv->fd, AUDIOIOC_FLUSH, 0);
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

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_CONFIGURE, &caps_desc);
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

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_CONFIGURE, &caps_desc);
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

long ff_nuttx_get_position(NuttxPriv *priv)
{
    long pos;
    int ret;

    ret = ff_nuttx_ioctl(priv->fd, AUDIOIOC_GETPOSITION, &pos);
    if (ret < 0)
        return ret;

    return pos;
}
