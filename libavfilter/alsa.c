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
 * alsa common handle
 */

#include <libavcodec/avcodec.h>
#include <libavutil/avassert.h>
#include <libavutil/avconfig.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "alsa.h"

static snd_pcm_format_t smpfmt_to_alsafmt(enum AVSampleFormat smpfmt,
                                          snd_pcm_access_t *access)
{
    switch (smpfmt) {
    case AV_SAMPLE_FMT_U8P:
        *access = SND_PCM_ACCESS_RW_NONINTERLEAVED;
    case AV_SAMPLE_FMT_U8:
        return SND_PCM_FORMAT_U8;
    case AV_SAMPLE_FMT_S16P:
        *access = SND_PCM_ACCESS_RW_NONINTERLEAVED;
    case AV_SAMPLE_FMT_S16:
        return SND_PCM_FORMAT_S16;
    case AV_SAMPLE_FMT_S32P:
        *access = SND_PCM_ACCESS_RW_NONINTERLEAVED;
    case AV_SAMPLE_FMT_S32:
        return SND_PCM_FORMAT_S32;
    case AV_SAMPLE_FMT_FLTP:
        *access = SND_PCM_ACCESS_RW_NONINTERLEAVED;
    case AV_SAMPLE_FMT_FLT:
        return SND_PCM_FORMAT_FLOAT;
    default:
        return SND_PCM_FORMAT_UNKNOWN;
    }
}

static enum AVSampleFormat alsafmt_to_smpfmt(snd_pcm_format_t pcmfmt)
{
    switch (pcmfmt) {
    case AUDIO_SUBFMT_PCM_U8:
        return AV_SAMPLE_FMT_U8;
    case AUDIO_SUBFMT_PCM_S16_LE:
    case AUDIO_SUBFMT_PCM_S16_BE:
        return AV_SAMPLE_FMT_S16;
    case AUDIO_SUBFMT_PCM_S32_LE:
    case AUDIO_SUBFMT_PCM_S32_BE:
        return AV_SAMPLE_FMT_S32;
    default:
        return AV_SAMPLE_FMT_NONE;
    }
}

static int alsa_fmt_to_avcodec(int fmt, int subfmt, int streamfmt)
{
    switch (fmt) {
        case AUDIO_FMT_PCM:
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

            break;
        case AUDIO_FMT_MP3: return AV_CODEC_ID_MP3;
        case AUDIO_FMT_AC3: return AV_CODEC_ID_AC3;
        case AUDIO_FMT_WMA: return AV_CODEC_ID_WMAV2;
        case AUDIO_FMT_DTS: return AV_CODEC_ID_DTS;
        case AUDIO_FMT_OGG_VORBIS: return AV_CODEC_ID_VORBIS;
        case AUDIO_FMT_FLAC: return AV_CODEC_ID_FLAC;
        case AUDIO_FMT_SBC:
            if (streamfmt == AUDIO_STREAMFORMAT_SBC_PACKED)
                return AV_CODEC_ID_SBC_PACKED_A2DP;
            return AV_CODEC_ID_SBC;
        case AUDIO_FMT_AMR: return AV_CODEC_ID_AMR_NB;
        case AUDIO_FMT_AMRWB:return AV_CODEC_ID_AMR_WB;
        case AUDIO_FMT_AAC:
            if (streamfmt == AUDIO_STREAMFORMAT_LATM)
                return AV_CODEC_ID_AAC_LATM_A2DP;
            return AV_CODEC_ID_AAC;
        case AUDIO_FMT_OPUS: return AV_CODEC_ID_OPUS;
        case AUDIO_FMT_LC3:  return AV_CODEC_ID_LC3;
        case AUDIO_FMT_SPEEX: return AV_CODEC_ID_SPEEX;
    }

    return AV_CODEC_ID_NONE;
}

static int alsa_samplerate_convert(int samplerate, int *sample_rates, int num)
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

static int alsa_ioctl(int fd, int cmd, unsigned long arg)
{
    int ret;

    ret = ioctl(fd, cmd, arg);
    if (ret < 0) {
        ret = -errno;
    }

    return ret;
}

static int alsa_get_capabilities(const char *device, int ac_type,
                                 int ac_subtype, struct audio_caps_s *caps)
{
    char path[32];
    int ret;
    int fd;

    snprintf(path, sizeof(path), CONFIG_AUDIOUTILS_ALSA_LIB_DEV_PATH "/%s", device);
    fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return -ENOENT;

    caps->ac_len = sizeof(struct audio_caps_s);
    caps->ac_type = ac_type;
    caps->ac_subtype = ac_subtype;

    ret = alsa_ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)caps);
    close(fd);

    return ret;
}

static int alsa_get_audio_info(const char *device, struct audio_info_s *info)
{
    char path[32];
    int ret;
    int fd;

    snprintf(path, sizeof(path), CONFIG_AUDIOUTILS_ALSA_LIB_DEV_PATH "/%s", device);
    fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return -ENOENT;

    ret = alsa_ioctl(fd, AUDIOIOC_GETAUDIOINFO, (unsigned long)info);
    close(fd);

    return ret;
}

int alsa_open(AlsaHandle *s, const char *device, snd_pcm_stream_t mode,
              int rate, AVChannelLayout ch_layout, enum AVSampleFormat smpfmt,
              int periods, int period_time)
{
    snd_pcm_access_t access = SND_PCM_ACCESS_RW_INTERLEAVED;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_format_t format;
    snd_pcm_t *h;
    int res;

    format = smpfmt_to_alsafmt(smpfmt, &access);
    if (format == SND_PCM_FORMAT_UNKNOWN) {
        av_log(NULL, AV_LOG_ERROR, "sample format %d is not supported\n", smpfmt);
        return AVERROR(ENOSYS);
    }
    s->frame_size = snd_pcm_format_width(format) / 8 * ch_layout.nb_channels;

    res = snd_pcm_open(&h, device, mode, SND_PCM_NONBLOCK);
    if (res < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot open audio device %s (%s)\n",
               device, snd_strerror(res));
        return AVERROR(EIO);
    }

    snd_pcm_hw_params_alloca(&hw_params);

    res = snd_pcm_hw_params_any(h, hw_params);
    if (res < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot initialize hardware parameter structure (%s)\n",
               snd_strerror(res));
        goto fail;
    }

    res = snd_pcm_hw_params_set_access(h, hw_params, access);
    if (res < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot set access type (%s)\n",
               snd_strerror(res));
        goto fail;
    }

    res = snd_pcm_hw_params_set_format(h, hw_params, format);
    if (res < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot set sample format %d %d (%s)\n",
               smpfmt, format, snd_strerror(res));
        goto fail;
    }

    res = snd_pcm_hw_params_set_rate(h, hw_params, rate, 0);
    if (res < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot set sample rate (%s)\n",
               snd_strerror(res));
        goto fail;
    }

    res = snd_pcm_hw_params_set_channels(h, hw_params, ch_layout.nb_channels);
    if (res < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot set channel count to %d (%s)\n",
               ch_layout.nb_channels, snd_strerror(res));
        goto fail;
    }

    snd_pcm_hw_params_set_period_time(h, hw_params, period_time * 1000, 0);
    snd_pcm_hw_params_set_periods(h, hw_params, periods, 0);

    res = snd_pcm_hw_params(h, hw_params);
    if (res < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot set parameters (%s)\n",
               snd_strerror(res));
        goto fail;
    }

    s->h           = h;
    s->ch_layout   = ch_layout;
    s->format      = smpfmt;
    s->sample_rate = rate;
    s->period_time = period_time;
    s->periods     = periods;
    return 0;

fail:
    snd_pcm_close(h);
    return AVERROR(EIO);
}

int alsa_close(AlsaHandle *s)
{
    snd_pcm_close(s->h);
    av_frame_free(&s->last_frame);
    av_channel_layout_uninit(&s->ch_layout);
    s->sample_rate = 0;
    s->period_time = 0;
    s->periods     = 0;
    s->h           = NULL;
    s->format      = AV_SAMPLE_FMT_NONE;

    return 0;
}

static int alsa_xrun_recover(snd_pcm_t *handle, int err)
{
    if (err == -EPIPE) {
        av_log(NULL, AV_LOG_WARNING, "ALSA buffer XRUN ret=%d.\n", err);
        err = snd_pcm_prepare(handle);
        if (err < 0) {
            av_log(NULL, AV_LOG_ERROR, "cannot recover from underrun (snd_pcm_prepare failed: %s)\n", snd_strerror(err));
            return AVERROR(EIO);
        }
    } else if (err == -ESTRPIPE) {
        av_log(NULL, AV_LOG_ERROR, "-ESTRPIPE... Unsupported!\n");
        return AVERROR(ESTRPIPE);
    } else if (err == -EAGAIN) {
        return 0;
    }

    return err;
}

int alsa_write(AlsaHandle *s, void **bufs, int size)
{
    int ret;

    while (1) {
        if (bufs[1])
            ret = snd_pcm_writen(s->h, bufs, size);
        else
            ret = snd_pcm_writei(s->h, bufs[0], size);

        if (ret >= 0 || ret == -EAGAIN)
            break;

        if (alsa_xrun_recover(s->h, ret) < 0)
            break;
    }

    return ret;
}

int alsa_read(AlsaHandle *s, void *buffer, int size)
{
    int ret;

    ret = snd_pcm_readi(s->h, buffer, size);
    if (ret < 0) {
        if (alsa_xrun_recover(s->h, ret) < 0)
            return ret;
        ret = snd_pcm_readi(s->h, buffer, size);
    }

    return ret;
}

static int alsa_set_ranges(struct AVOptionRanges *ranges, int nb_ranges,
                           int is_range, int min_v[], int max_v[])
{
    ranges->nb_components = 1;
    ranges->nb_ranges = nb_ranges;

    ranges->range = av_mallocz(nb_ranges * sizeof(AVOptionRange*));
    if (!ranges->range)
        return AVERROR(ENOMEM);

    for (int i = 0; i < nb_ranges; i++) {
        ranges->range[i] = av_mallocz(sizeof(AVOptionRange));
        if (!ranges->range[i])
            return AVERROR(ENOMEM);

        ranges->range[i]->is_range = is_range;
        ranges->range[i]->value_min = min_v[i];
        ranges->range[i]->value_max = is_range ? max_v[i] : min_v[i];
    }

    return 0;
}

static int alsa_capbility_query_smpfmts(const char *device, int format, int values[])
{
    struct audio_caps_s smpfmts;
    int ret, x, count;

    if (((format & (1 << (AUDIO_FMT_PCM - 1))) == 0))
        return AVERROR(EPERM);

    ret = alsa_get_capabilities(device, AUDIO_TYPE_QUERY, AUDIO_FMT_PCM, &smpfmts);
    if (ret < 0)
        return ret;

    count = 0;
    for (x = 0; x < sizeof(smpfmts.ac_controls.b); x++) {
        if (smpfmts.ac_controls.b[x] == AUDIO_SUBFMT_END)
            break;

        ret = alsafmt_to_smpfmt(smpfmts.ac_controls.b[x]);
        if (ret >= 0)
            values[count++] = ret;
    }

    return count == 0 ? AVERROR(EPERM) : count;
}

int alsa_query_caps(struct AVOptionRanges **pranges, const char *device,
                    const char *key, bool playback)
{
    int values0[64] = {0}, values1[64] = {0};
    struct audio_caps_s formats, others;
    struct audio_info_s info = {0};
    int ac_type = AUDIO_TYPE_QUERY;
    struct AVOptionRanges *ranges;
    int nb_ranges, is_range = 0;
    int format;
    int ret;

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        return AVERROR(ENOMEM);

    alsa_get_audio_info(device, &info);

    if (!strcmp(key, "sample_fmts")) {
        if (info.format > 0) {
            format = alsafmt_to_smpfmt(info.subformat);
            values0[0] = format;
            values1[0] = format;
            nb_ranges = 1;
        } else {
            ret = alsa_get_capabilities(device, ac_type, AUDIO_TYPE_QUERY, &formats);
            if (ret < 0)
                goto err;

            ret = alsa_capbility_query_smpfmts(device, formats.ac_format.hw, values0);
            if (ret < 0)
                goto err;

            nb_ranges = ret;
        }
    } else if (!strcmp(key, "channels") || !strcmp(key, "sample_rates")) {
        if (info.channels > 0 || info.samplerate > 0) {
            if (!strcmp(key, "channels")) {
                values0[0] = info.channels;
                values1[0] = info.channels;
                nb_ranges = 1;
            } else {
                values0[0] = info.samplerate;
                values1[0] = info.samplerate;
                nb_ranges = 1;
            }

        } else {
            ac_type = playback ? AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
            ret = alsa_get_capabilities(device, ac_type, AUDIO_TYPE_QUERY, &others);
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
                is_range = (values0[0] != values1[0]);
            } else {
                ret = alsa_samplerate_convert(others.ac_controls.hw[0], values0, 64);
                if (ret < 0)
                    goto err;

                nb_ranges = ret;
            }
        }
    }  else if (!strcmp(key, "codec")) {
        if (info.format > 0)
            format = alsa_fmt_to_avcodec(info.format, info.subformat, info.codec.format);
        else
            format = AV_CODEC_ID_FIRST_AUDIO;

        values0[0] = format;
        values1[0] = format;
        nb_ranges = 1;
    } else {
        ret = -EINVAL;
        goto err;
    }

    ret = alsa_set_ranges(ranges, nb_ranges, is_range, values0, values1);
    if (ret < 0)
        goto err;

    *pranges = ranges;
    return ranges->nb_components;

err:
    av_opt_freep_ranges(&ranges);
    return ret;
}

int alsa_set_parameter(const char *device, const char *parameter)
{
    char path[32];
    int ret;
    int fd;

    snprintf(path, sizeof(path), CONFIG_AUDIOUTILS_ALSA_LIB_DEV_PATH "/%s", device);
    fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return -ENOENT;

    ret = alsa_ioctl(fd, AUDIOIOC_SETPARAMTER, (unsigned long)parameter);
    close(fd);

    return ret;
}
