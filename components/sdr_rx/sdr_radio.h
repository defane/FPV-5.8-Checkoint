/* Target radio backend interface. GPL-3.0-or-later.
 * Everything above this interface is target independent.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to Lëtz Timer from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/radio/radio.h.
 * Changes for Lëtz Timer: the C5 backend only; no caps, regwatch, idle/sleep, tune-skip
 * or S3 API; a forced gain only; the split capture errors, the settle wait and the bank layout
 * (spec 2.3); esp_err.h only on the target, so the host tests can include it.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only.
 *
 * Private to components/sdr_rx. [scan]: only the task that owns the engine calls these. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_err.h"
#endif

/* Packed raw sample word: I bits 0..9, Q bits 10..19 (signed 10-bit). */
static inline int radio_word_i(uint32_t w) { return (int32_t)(w << 22) >> 22; }
static inline int radio_word_q(uint32_t w) { return (int32_t)(w << 12) >> 22; }
#define RADIO_FULL_SCALE 512

#define RADIO_MAX_RATES 6

/* The RF dump owns the whole 128 KiB SRAM bank 1 while a capture runs (spec 2.3). Only the
 * scan task touches it, never during a capture; the lower half survives captures. */
#define SDR_BANK_START       0x40820000u
#define SDR_BANK_PSD_WS      0x40820000u /* psd workspace (sdr_psd.c, PSD_WS_BYTES used) */
#define SDR_BANK_PSD_BYTES   0x1800u
#define SDR_BANK_GAIN        0x40821800u /* 8 gain slots (sdr_radio_c5.c) */
#define SDR_BANK_GAIN_BYTES  0x2000u
#define SDR_BANK_CAL         0x40823800u /* 16 calibration cache slots (sdr_recal_c5.c) */
#define SDR_BANK_CAL_BYTES   0x1800u
#define SDR_BANK_BENCH       0x40825000u /* bench scratch (sdr_bench_scratch) */
#define SDR_BANK_BENCH_BYTES 0xb000u
#define SDR_BANK_IQ          0x40830000u /* the capture buffer: the dump writes here */
#define SDR_BANK_END         0x40840000u

#ifdef ESP_PLATFORM
/* Releases the bank (a reset can leave the dump enabled and the modem owning it), then the gain
 * slots and the calibration cache in it. No tune: the caller tunes (and so calibrates) its LOs. */
esp_err_t radio_init(void);
#endif
const char *radio_phy_version(void);

#ifdef ESP_PLATFORM
/* Tune the receive LO (whole MHz, 5 GHz only). The first visit of an LO calibrates it
 * (~41 ms) and caches the calibration; later visits restore it. Applies the gain and the RX
 * path. Returns without waiting for the LO to settle: radio_capture does that, so other work
 * can run meanwhile. ESP_ERR_INVALID_ARG below 4900 MHz, ESP_ERR_INVALID_STATE once shut down. */
esp_err_t radio_tune(uint32_t lo_khz);
/* Forget one LO's cached calibration and calibrate it again (the FULL verify's retries). */
esp_err_t radio_recal_lo(uint32_t lo_khz);
#endif
/* Every cached calibration forgotten: the next tune of each LO calibrates it. */
void radio_force_recal(void);

/* -1 = PHY calibrated automatic filter. Applied around each capture only. */
void radio_set_filter(int code);
/* The requested gain index, re-applied by every tune (clamped to radio_gain_max() per apply,
 * the request itself is kept). No apply here: see radio_gain_now. */
void radio_set_gain(unsigned code);
unsigned radio_gain(void);
unsigned radio_gain_max(void);                /* the AGC table's maximum; reads the gain register */
#ifdef ESP_PLATFORM
/* Force this gain now, without a tune (under the PHY lock, low/high pulse, settle restarted).
 * A code other than radio_gain() is temporary: the next tune re-applies the request. */
esp_err_t radio_gain_now(unsigned code);

/* Blocking snapshot, taken once settle_us have passed since the last tune or gain change.
 * *words stays valid until the next capture; a tune leaves it alone. t_mid_us: the middle of
 * the capture; elapsed_us: the dump; waited_us: the settle wait. Errors: ESP_ERR_TIMEOUT,
 * ESP_ERR_INVALID_SIZE (short), ESP_ERR_INVALID_STATE (overrun, or shut down) and
 * ESP_ERR_INVALID_RESPONSE (stale: PHY tracking recalibrated since the tune; nothing captured,
 * re-tune the LO). */
esp_err_t radio_capture(unsigned n, unsigned rate_code, uint32_t settle_us, const uint32_t **words,
                        int64_t *t_mid_us, uint32_t *elapsed_us, uint32_t *waited_us);
#endif
uint32_t radio_rate_hz(unsigned rate_code);   /* 0 = unsupported */

/* [any] */
bool radio_calibrating(void);                 /* an LO calibration is running */
uint32_t radio_recals(void);                  /* LO calibrations since boot */
unsigned radio_foreign_dc(void);              /* c5_foreign_dc_count() */

/* Bench only (sdr_rx_bench.h). */
void radio_set_full_tune(bool on);            /* channel setup on every tune (TUNESKIP 0) */
#ifdef ESP_PLATFORM
esp_err_t radio_borrow_cal(uint32_t lo_khz, uint32_t cal_from_khz);
#endif
void radio_borrow_end(void);

/* The capture aperture, for scratch use between captures (it is overwritten
 * by the next radio_capture). */
uint32_t *radio_sample_buffer(unsigned *max_words);
/* The psd workspace and the bench scratch in the bank's lower half: captures never write them;
 * not accessible during radio_capture. */
void *radio_dsp_region(size_t *bytes);
void *radio_bench_region(size_t *bytes);
