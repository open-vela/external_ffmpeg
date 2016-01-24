/*
 * Copyright (c) 2010 Mans Rullgard <mans@mansr.com>
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

#include "config.h"

#include "libavutil/aarch64/cpu.h"
#include "libavutil/attributes.h"
#include "libavutil/internal.h"
#include "libavcodec/dcadsp.h"

void ff_dca_lfe_fir0_neon(float *out, const float *in, const float *coefs);
void ff_dca_lfe_fir1_neon(float *out, const float *in, const float *coefs);

av_cold void ff_dcadsp_init_aarch64(DCADSPContext *s)
{
    int cpu_flags = av_get_cpu_flags();

    if (have_neon(cpu_flags)) {
        s->lfe_fir[0] = ff_dca_lfe_fir0_neon;
        s->lfe_fir[1] = ff_dca_lfe_fir1_neon;
    }
}
