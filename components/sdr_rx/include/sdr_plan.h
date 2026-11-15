/* Enabled channels -> list of LO hops. GPL-3.0-or-later.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to FPV 5.8 Checkpoint from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/scan/planner.h.
 * Changes for FPV 5.8 Checkpoint: no band profile, ISM plan or enable mask (a config struct
 * carries the c5-race span/offset/step of profile.c); each channel's FFT bin window precomputed.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only.
 *
 * Pure C (host-tested in test/test_sdr_plan.c). */
#pragma once
#include "sdr_types.h"                /* the header's only include */

typedef struct {
    uint32_t hop_max_span_khz;    /* 40000 [W profile.c:62]: widest pair under one LO */
    int32_t  single_lo_offset_khz;/* -8000 [W profile.c:63] */
    uint32_t lo_step_khz;         /* 1000  [W profile.c:64]: the C5 tunes whole MHz */
} sdr_plan_cfg_t;
typedef struct { int16_t r0, r1; } sdr_band_t;       /* inclusive FFT bin window, natural order */
typedef struct {
    uint32_t lo_khz;
    uint8_t nch, ch[SDR_RX_CHANNELS];
    int32_t offset_hz[SDR_RX_CHANNELS];              /* channel - LO */
    sdr_band_t band[SDR_RX_CHANNELS];                /* filled by sdr_plan_bands() */
} sdr_plan_hop_t;
typedef struct { uint8_t nhops; sdr_plan_hop_t hop[SDR_RX_MAX_HOPS]; } sdr_plan_t;

sdr_plan_cfg_t sdr_plan_cfg_default(void);
/* widscan58 plan_build/place_lo [W planner.c:5-102], minus the profile and ISM. Channels
 * sorted by frequency, hops in ascending LO order.
 * Pairs mode groups the channels (sorted by frequency) into as few LOs as possible, each group
 * spanning at most hop_max_span_khz; the LO goes where its DC spike sits farthest from every
 * channel in the group. Single mode gives each channel its own LO at single_lo_offset_khz.
 * ch[] holds indices into ch_khz (at most SDR_RX_CHANNELS of them are planned). */
void sdr_plan_build(const uint32_t *ch_khz, unsigned n, sdr_plan_mode_t mode,
                    const sdr_plan_cfg_t *cfg, sdr_plan_t *out);
/* Bins whose centre is within +-half_hz of offset_hz: r0 = nfft/2 + ceil((off-half)/bin),
 * r1 = nfft/2 + floor((off+half)/bin), clamped to [0, nfft-1], bin = rate_hz/nfft, all
 * integer (as metrics_level [W metrics.c:236-240] without floats). */
sdr_band_t sdr_plan_band(unsigned nfft, uint32_t rate_hz, int32_t offset_hz, uint32_t half_hz);
void sdr_plan_bands(sdr_plan_t *p, unsigned nfft, uint32_t rate_hz, uint32_t half_hz);
