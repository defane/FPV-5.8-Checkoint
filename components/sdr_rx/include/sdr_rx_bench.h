/* sdr_rx bench API: what the console benches (components/checkpoint) use beyond sdr_rx.h.
 * [scan] only, and never during production: every call here runs in the scan task, between
 * sdr_rx_step calls, and leaves the engine for the next sdr_rx_step to pick up (it re-tunes
 * the hop's LO first). End a bench with sdr_bench_set_tune_method(SDR_TUNE_PLL) and
 * sdr_bench_set_plan(SDR_PLAN_PAIRS), which also resets the IQ states. GPL-3.0-only. */
#pragma once
#include "sdr_rx.h"
#include "sdr_plan.h"                 /* public (include/), so components/checkpoint can use sdr_plan_t */
typedef enum { SDR_TUNE_PLL = 0, SDR_TUNE_FULL = 1 } sdr_tune_method_t;
typedef struct {                      /* a view of the last psd; valid until the next sdr_bench_psd/step */
    const uint32_t *p; const uint8_t *valid; float scale, enbw_bins, dc_i, dc_q;
    unsigned nfft, segments, saturated; bool iq_applied; uint32_t us;
} sdr_psd_view_t;
void sdr_bench_set_tune_method(sdr_tune_method_t m);          /* FULL = channel setup every tune */
esp_err_t sdr_bench_tune(uint32_t lo_khz, uint32_t *tune_us); /* radio_tune + timing */
esp_err_t sdr_bench_capture(unsigned n, uint32_t settle_us, const uint32_t **words,
                            int64_t *t_mid_us, uint32_t *cap_us);
/* iq_lo: plan hop whose psd_iq_t to use and update, -1 = no correction */
esp_err_t sdr_bench_psd(const uint32_t *words, unsigned n, unsigned nfft, int iq_lo, sdr_psd_view_t *out);
int16_t sdr_bench_level(const sdr_psd_view_t *v, int32_t offset_hz, uint32_t half_hz, uint8_t *flags);
float sdr_bench_irr_db(int lo);                               /* psd_iq_irr_db of that LO's EMA state */
unsigned sdr_bench_moments_add(const uint32_t *words, unsigned n, sdr_moments_t *m);  /* psd_moments_add, nfft 64 */
float sdr_bench_moments_irr_db(const sdr_moments_t *m);       /* psd_iq_irr_db(psd_moments_mean(m)) */
/* ONCE emulation (positive control for CAL_NEEDED, K n): at lo_khz, restore cal_from_khz's
 * cached DC/IQ areas instead of lo's own, cal_gen++ (no gain slot keeps a table built from
 * them), full channel setup (no gain recording); after the caller's captures,
 * sdr_bench_borrow_end() does cal_gen++ and calibrated_mhz = 0 so the next tune restores
 * lo's own entry. ESP_ERR_NOT_FOUND if cal_from has no cache slot. */
esp_err_t sdr_bench_borrow_cal(uint32_t lo_khz, uint32_t cal_from_khz);
void sdr_bench_borrow_end(void);
const sdr_plan_t *sdr_bench_plan(void);
void sdr_bench_set_plan(sdr_plan_mode_t m);                   /* rebuild the plan (new LOs calibrate on first visit) */
void *sdr_bench_scratch(size_t *bytes);                       /* 44 KiB in the bank, scan task, not during a capture */
