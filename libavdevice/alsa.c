/*
 * ALSA input and output
 * Copyright (c) 2007 Luca Abeni ( lucabe72 email it )
 * Copyright (c) 2007 Benoit Fouet ( benoit fouet free fr )
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
 * ALSA input and output: common code
 * @author Luca Abeni ( lucabe72 email it )
 * @author Benoit Fouet ( benoit fouet free fr )
 * @author Nicolas George ( nicolas george normalesup org )
 */

#include "config_components.h"

#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "avdevice.h"
#include "libavutil/avassert.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"

#include "alsa.h"

static av_cold snd_pcm_format_t codec_id_to_pcm_format(int codec_id)
{
    switch(codec_id) {
    case AV_CODEC_ID_PCM_S32LE: return SND_PCM_FORMAT_S32_LE;
    case AV_CODEC_ID_PCM_S32BE: return SND_PCM_FORMAT_S32_BE;
    case AV_CODEC_ID_PCM_U32LE: return SND_PCM_FORMAT_U32_LE;
    case AV_CODEC_ID_PCM_U32BE: return SND_PCM_FORMAT_U32_BE;
    case AV_CODEC_ID_PCM_S16LE: return SND_PCM_FORMAT_S16_LE;
    case AV_CODEC_ID_PCM_S16BE: return SND_PCM_FORMAT_S16_BE;
    case AV_CODEC_ID_PCM_U16LE: return SND_PCM_FORMAT_U16_LE;
    case AV_CODEC_ID_PCM_U16BE: return SND_PCM_FORMAT_U16_BE;
    case AV_CODEC_ID_PCM_S8:    return SND_PCM_FORMAT_S8;
    case AV_CODEC_ID_PCM_U8:    return SND_PCM_FORMAT_U8;
    default:                 return SND_PCM_FORMAT_UNKNOWN;
    }
}

#define MAKE_REORDER_FUNC(NAME, TYPE, CHANNELS, LAYOUT, MAP)                \
static void alsa_reorder_ ## NAME ## _ ## LAYOUT(const void *in_v,          \
                                                 void *out_v,               \
                                                 int n)                     \
{                                                                           \
    const TYPE *in = in_v;                                                  \
    TYPE      *out = out_v;                                                 \
                                                                            \
    while (n-- > 0) {                                                       \
        MAP                                                                 \
        in  += CHANNELS;                                                    \
        out += CHANNELS;                                                    \
    }                                                                       \
}

#define MAKE_REORDER_FUNCS(CHANNELS, LAYOUT, MAP) \
    MAKE_REORDER_FUNC(int8,  int8_t,  CHANNELS, LAYOUT, MAP) \
    MAKE_REORDER_FUNC(int16, int16_t, CHANNELS, LAYOUT, MAP) \
    MAKE_REORDER_FUNC(int32, int32_t, CHANNELS, LAYOUT, MAP) \
    MAKE_REORDER_FUNC(f32,   float,   CHANNELS, LAYOUT, MAP)

MAKE_REORDER_FUNCS(5, out_50, \
        out[0] = in[0]; \
        out[1] = in[1]; \
        out[2] = in[3]; \
        out[3] = in[4]; \
        out[4] = in[2]; \
        )

MAKE_REORDER_FUNCS(6, out_51, \
        out[0] = in[0]; \
        out[1] = in[1]; \
        out[2] = in[4]; \
        out[3] = in[5]; \
        out[4] = in[2]; \
        out[5] = in[3]; \
        )

MAKE_REORDER_FUNCS(8, out_71, \
        out[0] = in[0]; \
        out[1] = in[1]; \
        out[2] = in[4]; \
        out[3] = in[5]; \
        out[4] = in[2]; \
        out[5] = in[3]; \
        out[6] = in[6]; \
        out[7] = in[7]; \
        )

#define FORMAT_I8  0
#define FORMAT_I16 1
#define FORMAT_I32 2
#define FORMAT_F32 3

#define PICK_REORDER(layout)\
switch(format) {\
    case FORMAT_I8:  s->reorder_func = alsa_reorder_int8_out_ ##layout;  break;\
    case FORMAT_I16: s->reorder_func = alsa_reorder_int16_out_ ##layout; break;\
    case FORMAT_I32: s->reorder_func = alsa_reorder_int32_out_ ##layout; break;\
    case FORMAT_F32: s->reorder_func = alsa_reorder_f32_out_ ##layout;   break;\
}

static av_cold int find_reorder_func(AlsaData *s, int codec_id, AVChannelLayout *layout, int out)
{
    int format;

    /* reordering input is not currently supported */
    if (!out)
        return AVERROR(ENOSYS);

    /* reordering is not needed for QUAD or 2_2 layout */
    if (!av_channel_layout_compare(layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_QUAD) ||
        !av_channel_layout_compare(layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_2_2))
        return 0;

    switch (codec_id) {
    case AV_CODEC_ID_PCM_S8:
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_ALAW:
    case AV_CODEC_ID_PCM_MULAW: format = FORMAT_I8;  break;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_U16LE:
    case AV_CODEC_ID_PCM_U16BE: format = FORMAT_I16; break;
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_S32BE:
    case AV_CODEC_ID_PCM_U32LE:
    case AV_CODEC_ID_PCM_U32BE: format = FORMAT_I32; break;
    case AV_CODEC_ID_PCM_F32LE:
    case AV_CODEC_ID_PCM_F32BE: format = FORMAT_F32; break;
    default:                 return AVERROR(ENOSYS);
    }

    if (!av_channel_layout_compare(layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT0_BACK) ||
        !av_channel_layout_compare(layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT0))
        PICK_REORDER(50)
    else if (!av_channel_layout_compare(layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1_BACK) ||
             !av_channel_layout_compare(layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1))
        PICK_REORDER(51)
    else if (!av_channel_layout_compare(layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1))
        PICK_REORDER(71)

    return s->reorder_func ? 0 : AVERROR(ENOSYS);
}

static int ff_audio_samplerate_convert(int samplerate, int *sample_rates, int num)
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

static int ff_audio_subfmt_to_avcodec(int subfmt)
{
    switch (subfmt) {
    case AUDIO_SUBFMT_PCM_U8:
        return AV_CODEC_ID_PCM_U8;
    case AUDIO_SUBFMT_PCM_S8:
        return AV_CODEC_ID_PCM_S8;
    case AUDIO_SUBFMT_PCM_U16_LE:
        return AV_CODEC_ID_PCM_U16LE;
    case AUDIO_SUBFMT_PCM_U16_BE:
        return AV_CODEC_ID_PCM_U16BE;
    case AUDIO_SUBFMT_PCM_S16_LE:
        return AV_CODEC_ID_PCM_S16LE;
    case AUDIO_SUBFMT_PCM_S16_BE:
        return AV_CODEC_ID_PCM_S16BE;
    case AUDIO_SUBFMT_PCM_U32_LE:
        return AV_CODEC_ID_PCM_U32LE;
    case AUDIO_SUBFMT_PCM_U32_BE:
        return AV_CODEC_ID_PCM_U32BE;
    case AUDIO_SUBFMT_PCM_S32_LE:
        return AV_CODEC_ID_PCM_S32LE;
    case AUDIO_SUBFMT_PCM_S32_BE:
        return AV_CODEC_ID_PCM_S32BE;
    case AUDIO_SUBFMT_PCM_MU_LAW:
        return AV_CODEC_ID_PCM_MULAW;
    case AUDIO_SUBFMT_PCM_A_LAW:
        return AV_CODEC_ID_PCM_ALAW;
    case AUDIO_SUBFMT_PCM_MP1:
        return AV_CODEC_ID_MP1;
    case AUDIO_SUBFMT_PCM_MP2:
        return AV_CODEC_ID_MP2;
    case AUDIO_SUBFMT_PCM_MP3:
        return AV_CODEC_ID_MP3;
    }

    return AV_CODEC_ID_FIRST_AUDIO;
}

static int ff_audio_subfmt_to_smpfmt(int subfmt)
{
    switch (subfmt) {
    case AUDIO_SUBFMT_PCM_U8:
        return AV_SAMPLE_FMT_U8;
    case AUDIO_SUBFMT_PCM_S16_LE:
    case AUDIO_SUBFMT_PCM_S16_BE:
        return AV_SAMPLE_FMT_S16;
    case AUDIO_SUBFMT_PCM_S32_LE:
    case AUDIO_SUBFMT_PCM_S32_BE:
        return AV_SAMPLE_FMT_S32;
    }

    return AV_SAMPLE_FMT_NONE;
}

static int ff_audio_fmt_to_avcodec(int *codec_id, int *formats)
{
    int codec = AV_CODEC_ID_NONE;
    int format = AUDIO_FMT_UNDEF;

    if (*formats & (1 << (AUDIO_FMT_PCM - 1))) {
        codec = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE);
        format = AUDIO_FMT_PCM;
        *formats &= ~(1 << (AUDIO_FMT_PCM - 1));
    } else if (*formats & (1 << (AUDIO_FMT_MP3 - 1))) {
        codec = AV_CODEC_ID_MP3;
        format = AUDIO_FMT_MP3;
        *formats &= ~(1 << (AUDIO_FMT_MP3 - 1));
    } else if (*formats & (1 << (AUDIO_FMT_AC3 - 1))) {
        codec = AV_CODEC_ID_AC3;
        format = AUDIO_FMT_AC3;
        *formats &= ~(1 << (AUDIO_FMT_AC3 - 1));
    } else if (*formats & (1 << (AUDIO_FMT_WMA - 1))) {
        codec = AV_CODEC_ID_WMAV2;
        format = AUDIO_FMT_WMA;
        *formats &= ~(1 << (AUDIO_FMT_WMA - 1));
    } else if (*formats & (1 << (AUDIO_FMT_DTS - 1))) {
        codec = AV_CODEC_ID_DTS;
        format = AUDIO_FMT_WMA;
        *formats &= ~(1 << (AUDIO_FMT_DTS - 1));
    } else if (*formats & (1 << (AUDIO_FMT_OGG_VORBIS - 1))) {
        codec = AV_CODEC_ID_VORBIS;
        format = AUDIO_FMT_OGG_VORBIS;
        *formats &= ~(1 << (AUDIO_FMT_OGG_VORBIS - 1));
    } else if (*formats & (1 << (AUDIO_FMT_FLAC - 1))) {
        codec = AV_CODEC_ID_FLAC;
        format = AUDIO_FMT_FLAC;
        *formats &= ~(1 << (AUDIO_FMT_FLAC - 1));
    } else if (*formats & (1 << (AUDIO_FMT_AMR - 1))) {
        codec = AV_CODEC_ID_AMR_NB;
        format = AUDIO_FMT_AMR;
        *formats &= ~(1 << (AUDIO_FMT_AMR - 1));
    } else if (*formats & (1 << (AUDIO_FMT_OTHER - 1))) {
        format = AUDIO_FMT_OTHER;
        *formats &= ~(1 << (AUDIO_FMT_OTHER - 1));
    } else if (*formats & (1 << (AUDIO_FMT_OPUS - 1))) {
        codec = AV_CODEC_ID_OPUS;
        format = AUDIO_FMT_OPUS;
        *formats &= ~(1 << (AUDIO_FMT_OPUS - 1));
    } else if (*formats & (1 << (AUDIO_FMT_AMRWB - 1))) {
        codec = AV_CODEC_ID_AMR_WB;
        format = AUDIO_FMT_AMRWB;
        *formats &= ~(1 << (AUDIO_FMT_AMRWB - 1));
    }

    *codec_id = codec;

    return format;
}

static int ff_audio_pcm_ioctl(int fd, int cmd, unsigned long arg)
{
    int ret;

    ret = ioctl(fd, cmd, arg);
    if (ret < 0) {
        ret = -errno;
    }

    return ret;
}

#define ff_audio_pcm_ioctl(fd, cmd, arg) \
    ff_audio_pcm_ioctl(fd, cmd, (unsigned long)(arg))

static int ff_audio_get_capabilities(char *device, int ac_type, int ac_subtype,
    struct audio_caps_s *caps)
{
    int ret;
    int fd;
    char path[32];

    snprintf(path, sizeof(path), CONFIG_AUDIOUTILS_ALSA_LIB_DEV_PATH "/%s", device);
    fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0)
        return -ENOENT;

    caps->ac_len = sizeof(struct audio_caps_s);
    caps->ac_type = ac_type;
    caps->ac_subtype = ac_subtype;

    ret = ff_audio_pcm_ioctl(fd, AUDIOIOC_GETCAPS, caps);
    close(fd);
    return ret;
}

av_cold int ff_alsa_open(AVFormatContext *ctx, snd_pcm_stream_t mode,
                         unsigned int *sample_rate,
                         int channels, enum AVCodecID *codec_id)
{
    AlsaData *s = ctx->priv_data;
    AVChannelLayout *layout = &ctx->streams[0]->codecpar->ch_layout;
    const char *audio_device;
    int res, dir = 0;
    snd_pcm_format_t format;
    snd_pcm_t *h;
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_uframes_t buffer_size, period_size;

    if (ctx->url[0] == 0) audio_device = "default";
    else                  audio_device = ctx->url;

    if (*codec_id == AV_CODEC_ID_NONE)
        *codec_id = DEFAULT_CODEC_ID;
    format = codec_id_to_pcm_format(*codec_id);
    if (format == SND_PCM_FORMAT_UNKNOWN) {
        av_log(ctx, AV_LOG_ERROR, "sample format 0x%04x is not supported\n", *codec_id);
        return AVERROR(ENOSYS);
    }
    s->frame_size = av_get_bits_per_sample(*codec_id) / 8 * channels;

    res = snd_pcm_open(&h, audio_device, mode, SND_PCM_NONBLOCK);
    if (res < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot open audio device %s (%s)\n",
               audio_device, snd_strerror(res));
        return AVERROR(EIO);
    }

    snd_pcm_hw_params_alloca(&hw_params);

    res = snd_pcm_hw_params_any(h, hw_params);
    if (res < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot initialize hardware parameter structure (%s)\n",
               snd_strerror(res));
        goto fail;
    }

    res = snd_pcm_hw_params_set_access(h, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (res < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot set access type (%s)\n",
               snd_strerror(res));
        goto fail;
    }

    res = snd_pcm_hw_params_set_format(h, hw_params, format);
    if (res < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot set sample format 0x%04x %d (%s)\n",
               *codec_id, format, snd_strerror(res));
        goto fail;
    }

    res = snd_pcm_hw_params_set_rate_near(h, hw_params, sample_rate, 0);
    if (res < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot set sample rate (%s)\n",
               snd_strerror(res));
        goto fail;
    }

    res = snd_pcm_hw_params_set_channels(h, hw_params, channels);
    if (res < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot set channel count to %d (%s)\n",
               channels, snd_strerror(res));
        goto fail;
    }

    snd_pcm_hw_params_set_period_time(h, hw_params, s->period_time * 1000, dir);
    snd_pcm_hw_params_set_periods(h, hw_params, s->periods, dir);

    res = snd_pcm_hw_params(h, hw_params);
    if (res < 0) {
        av_log(ctx, AV_LOG_ERROR, "cannot set parameters (%s)\n",
               snd_strerror(res));
        goto fail;
    }

    if (channels > 2 && layout->order != AV_CHANNEL_ORDER_UNSPEC) {
        if (find_reorder_func(s, *codec_id, layout, mode == SND_PCM_STREAM_PLAYBACK) < 0) {
            char name[128];
            av_channel_layout_describe(layout, name, sizeof(name));
            av_log(ctx, AV_LOG_WARNING, "ALSA channel layout unknown or unimplemented for %s %s.\n",
                   name, mode == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture");
        }
        if (s->reorder_func) {
            s->reorder_buf_size = buffer_size;
            s->reorder_buf = av_malloc_array(s->reorder_buf_size, s->frame_size);
            if (!s->reorder_buf)
                goto fail;
        }
    }

    s->pkt = av_packet_alloc();
    if (!s->pkt)
        goto fail;

    s->h = h;
    return 0;

fail:
    snd_pcm_close(h);
    return AVERROR(EIO);
}

av_cold int ff_alsa_close(AVFormatContext *s1)
{
    AlsaData *s = s1->priv_data;

    snd_pcm_drain(s->h);
    av_freep(&s->reorder_buf);
    if (CONFIG_ALSA_INDEV)
        ff_timefilter_destroy(s->timefilter);
    snd_pcm_close(s->h);
    av_packet_free(&s->pkt);
    return 0;
}

int ff_alsa_xrun_recover(AVFormatContext *s1, int err)
{
    AlsaData *s = s1->priv_data;
    snd_pcm_t *handle = s->h;

    av_log(s1, AV_LOG_WARNING, "ALSA buffer xrun ret=%d.\n", err);
    if (err == -EPIPE) {
        err = snd_pcm_prepare(handle);
        if (err < 0) {
            av_log(s1, AV_LOG_ERROR, "cannot recover from underrun (snd_pcm_prepare failed: %s)\n", snd_strerror(err));

            return AVERROR(EIO);
        }
    } else if (err == -ESTRPIPE) {
        av_log(s1, AV_LOG_ERROR, "-ESTRPIPE... Unsupported!\n");

        return -1;
    } else if (err == -EAGAIN) {
        return 0;
    }
    return err;
}

int ff_alsa_extend_reorder_buf(AlsaData *s, int min_size)
{
    int size = s->reorder_buf_size;
    void *r;

    av_assert0(size != 0);
    while (size < min_size)
        size *= 2;
    r = av_realloc_array(s->reorder_buf, size, s->frame_size);
    if (!r)
        return AVERROR(ENOMEM);
    s->reorder_buf = r;
    s->reorder_buf_size = size;
    return 0;
}

/* ported from alsa-utils/aplay.c */
int ff_alsa_get_device_list(AVDeviceInfoList *device_list, snd_pcm_stream_t stream_type)
{
    return 0;
}

static int ff_audio_set_ranges(struct AVOptionRanges *ranges, int nb_ranges, int is_range,
    int min_v[], int max_v[])
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

static int ff_audio_capbility_query_smpfmts(struct AVFormatContext *s1, int format, int values[])
{
    struct audio_caps_s smpfmts;
    int ret, x;

    AlsaData *s = s1->priv_data;

    if (((format & (1 << (AUDIO_FMT_PCM - 1))) == 0))
        return AVERROR(EPERM);

    ret = ff_audio_get_capabilities(s1->url, AUDIO_TYPE_QUERY, AUDIO_FMT_PCM, &smpfmts);
    if (ret < 0)
        return ret;

    for (x = 0; x < sizeof(smpfmts.ac_controls.b); x++) {
        if (smpfmts.ac_controls.b[x] == AUDIO_SUBFMT_END)
            break;

        ret = ff_audio_subfmt_to_smpfmt(smpfmts.ac_controls.b[x]);
        if (ret >= 0)
            values[x] = ret;
    }

    return x == 0 ? AVERROR(EPERM) : x;
}

static int ff_audio_capbility_query_codecs(struct AVFormatContext *s1,
    int format, int codecs[], int num)
{
    int ac_subtype = AUDIO_FMT_UNDEF;
    int codec = AV_CODEC_ID_NONE;
    struct audio_caps_s caps;
    int i, nb_codecs = 0;

    AlsaData *s = s1->priv_data;

    while (nb_codecs < num && format) {
        ac_subtype = ff_audio_fmt_to_avcodec(&codec, &format);
        if (ff_audio_get_capabilities(s1->url, AUDIO_TYPE_QUERY, ac_subtype, &caps) < 0)
            continue;

        if (ac_subtype == AUDIO_FMT_OTHER) {
            nb_codecs += ff_audio_capbility_query_codecs(s1, caps.ac_controls.w, &codecs[nb_codecs], num - nb_codecs);
            continue;
        }

        for (i = 0; i < sizeof(caps.ac_controls.b) && nb_codecs < num; i++) {
            if (caps.ac_controls.b[i] == AUDIO_SUBFMT_END) {
                if (i == 0)
                    codecs[nb_codecs++] = codec;
                break;
            }

            codecs[nb_codecs++] = ff_audio_subfmt_to_avcodec(caps.ac_controls.b[i]);
        }
    }

    return nb_codecs;
}

int ff_audio_capbility_query_ranges(struct AVOptionRanges **ranges_, void *obj,
    const char *key, int flags, bool playback)
{
    struct AVFormatContext *s1 = obj;
    struct audio_caps_s formats, others;
    int ac_type = AUDIO_TYPE_QUERY;
    struct AVOptionRanges *ranges;
    int values0[64], values1[64];
    int nb_ranges, is_range = 0;
    int ret;

    AlsaData *s = s1->priv_data;

    ranges = av_mallocz(sizeof(struct AVOptionRanges));
    if (!ranges)
        return AVERROR(ENOMEM);

    if (!strcmp(key, "sample_fmts") || !strcmp(key, "codecs")) {
        ret = ff_audio_get_capabilities(s1->url, ac_type, AUDIO_TYPE_QUERY, &formats);
        if (ret < 0)
            goto err;

        if (!strcmp(key, "sample_fmts")) {
            ret = ff_audio_capbility_query_smpfmts(s1, formats.ac_format.hw, values0);
            if (ret < 0)
                goto err;
        } else {
            ret = ff_audio_capbility_query_codecs(s1, formats.ac_format.hw, values0, 64);
            if (ret < 0)
                goto err;
        }

        nb_ranges = ret;
    } else if (!strcmp(key, "channels") || !strcmp(key, "sample_rates")) {
        ac_type = playback ? AUDIO_TYPE_OUTPUT : AUDIO_TYPE_INPUT;
        ret = ff_audio_get_capabilities(s1->url, ac_type, AUDIO_TYPE_QUERY, &others);
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
            ret = ff_audio_samplerate_convert(others.ac_controls.hw[0], values0, 64);
            if (ret < 0)
                goto err;

            nb_ranges = ret;
        }
    } else {
        ret = -EINVAL;
        goto err;
    }

    ret = ff_audio_set_ranges(ranges, nb_ranges, is_range, values0, values1);
    if (ret < 0)
        goto err;

    *ranges_ = ranges;
    return ranges->nb_components;

err:
    av_opt_freep_ranges(&ranges);
    return ret;
}
