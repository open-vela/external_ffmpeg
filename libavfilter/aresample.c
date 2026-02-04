/*
 * Copyright (c) 2024 HiccupZhu
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
 * resampling audio module
 */

#include "aresample.h"
#include "libswresample/swresample.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "audio.h"

av_cold void ff_resample_init(AResampleContext *ar)
{
    if (!ar) {
        av_log(NULL, AV_LOG_ERROR, "AResampleContext is NULL\n");
        return;
    }
    memset(ar, 0, sizeof(*ar));
}

av_cold void ff_resample_uninit(AResampleContext *ar)
{
    if (!ar)
        return;

    if (ar->swr)
        swr_free(&ar->swr);
}

int ff_resample_frame(AResampleContext *ar, AVFilterLink *link, AVFrame *iframe, AVFrame **poframe)
{
    int64_t delay;
    AVFrame *oframe;
    int ret;
    int n_out;

    if (!ar || !link || !iframe) {
        av_log(NULL, AV_LOG_ERROR, "Invalid parameters: ar=%p, link=%p, iframe=%p\n",
               ar, link, iframe);
        return AVERROR(EINVAL);
    }
    n_out = iframe->nb_samples * link->sample_rate / iframe->sample_rate + 32;
    if (av_channel_layout_compare(&link->ch_layout, &iframe->ch_layout) == 0 &&
        link->format == iframe->format &&
        link->sample_rate == iframe->sample_rate)
    {
        *poframe = av_frame_alloc();
        if (!*poframe) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate frame\n");
            return AVERROR(ENOMEM);
        }

        ret = av_frame_ref(*poframe, iframe);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to reference frame: %s\n", av_err2str(ret));
            av_frame_free(poframe);
            return ret;
        }
        return 0;
    }

    if (!ar->swr ||
        av_channel_layout_compare(&ar->ch_layout, &iframe->ch_layout) ||
        ar->format != iframe->format ||
        ar->sample_rate != iframe->sample_rate)
    {
        if (ar->swr)
            swr_free(&ar->swr);

        ar->swr = swr_alloc();
        if (!ar->swr) {
            return AVERROR(ENOMEM);
        }

        ret = swr_alloc_set_opts2(&ar->swr, &link->ch_layout, link->format, link->sample_rate,
                                  &iframe->ch_layout, iframe->format, iframe->sample_rate, 0, NULL);
        if (ret < 0) {
            av_log(link->src, AV_LOG_ERROR, "Error swr set opts2 ret:%d:%s\n", ret, av_err2str(ret));
            swr_free(&ar->swr);
            return ret;
        }

        ret = av_opt_set_int(ar->swr, "filter_size", 16, 0);
        if (ret < 0) {
            av_log(link->src, AV_LOG_ERROR, "Error swr set filter_size ret:%d:%s\n", ret, av_err2str(ret));
            swr_free(&ar->swr);
            return ret;
        }

        ret = av_opt_set_int(ar->swr, "phase_shift", 6, 0);
        if (ret < 0) {
            av_log(link->src, AV_LOG_ERROR, "Error swr set phase_shift ret:%d:%s\n", ret, av_err2str(ret));
            swr_free(&ar->swr);
            return ret;
        }

        ret = av_opt_set_int(ar->swr, "tsf", AV_SAMPLE_FMT_S16P, 0);
        if (ret < 0) {
            av_log(link->src, AV_LOG_ERROR, "Error swr set tsf ret:%d:%s\n", ret, av_err2str(ret));
            swr_free(&ar->swr);
            return ret;
        }

        ret = swr_init(ar->swr);
        if (ret < 0) {
            av_log(link->src, AV_LOG_ERROR, "Error swr init ret:%d:%s\n", ret, av_err2str(ret));
            swr_free(&ar->swr);
            return ret;
        }

        av_channel_layout_copy(&ar->ch_layout, &iframe->ch_layout);
        ar->format = iframe->format;
        ar->sample_rate = iframe->sample_rate;
    }

    delay = swr_get_delay(ar->swr, link->sample_rate);
    if (delay > 0)
        n_out += FFMIN(delay, FFMAX(4096, n_out));

    oframe = ff_default_get_audio_buffer(link, n_out);
    if (!oframe) {
        return AVERROR(ENOMEM);
    }

    oframe->format = link->format;
    ret = av_channel_layout_copy(&oframe->ch_layout, &link->ch_layout);
    if (ret < 0) {
        av_frame_free(&oframe);
        return ret;
    }
    oframe->sample_rate = link->sample_rate;
    oframe->time_base = link->time_base;

    if(iframe->pts != AV_NOPTS_VALUE) {
        int64_t inpts = av_rescale(iframe->pts, iframe->time_base.num * (int64_t)oframe->sample_rate * iframe->sample_rate, iframe->time_base.den);
        int64_t outpts = swr_next_pts(ar->swr, inpts);
        oframe->pts = ROUNDED_DIV(outpts, oframe->sample_rate);
    } else {
        oframe->pts  = AV_NOPTS_VALUE;
    }
    oframe->pkt_dts = oframe->pts;

    n_out = swr_convert(ar->swr, oframe->extended_data, n_out,
                        (void *)iframe->extended_data, iframe->nb_samples);
    if (n_out <= 0) {
        av_frame_free(&oframe);
        return 0;
    }

    oframe->nb_samples = n_out;
    *poframe = oframe;

    return n_out;
}

int ff_resample_get_delay(AResampleContext *ar, int64_t base)
{
    if (!ar) {
        av_log(NULL, AV_LOG_ERROR, "AResampleContext is NULL\n");
        return AVERROR(EINVAL);
    }
    
    if (!ar->swr) {
        av_log(NULL, AV_LOG_DEBUG, "SwrContext not initialized\n");
        return 0;
    }

    return swr_get_delay(ar->swr, base);
}
