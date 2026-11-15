/* sdr_rx: the receive engine (see sdr_rx.h). One step is one hop of the LO plan: capture the
 * LO the previous step tuned, tune the next one, then the PSD and the band levels of the capture
 * while the new LO settles. After widscan58's scanner (CapnBry, https://github.com/CapnBry/widscan58,
 * a9fff97, firmware/src/scan/scanner.c do_hop, do_levels and hop_stats): no frames, spectra,
 * METRICS FULL, throttle, idle or RotorHazard; readings, a 1 s health window, requests, the
 * calibration of every LO and its image-rejection verify instead. GPL-3.0-only. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdr_level.h"
#include "sdr_plan.h"
#include "sdr_psd.h"
#include "sdr_radio.h"
#include "sdr_rx.h"
#include "sdr_rx_bench.h"

#define RATE_CODE 0u               /* 80 MS/s [W profile.c:54] */
#define RATE_HZ 80000000u
#define SEGMENTS 4u                /* capture = SEGMENTS x nfft [W proto/commands.c:96-112] */
#define USABLE_HZ 40000000.0f      /* Nyquist [W profile.c:58] */
#define DC_GUARD 1u                /* [W scanner.c:122] */
#define BAND_HALF_HZ 5000000u      /* the band power's half-width [W profile.c:60] */
#define CONJUGATE true             /* RF above the LO appears at negative baseband frequency */
#define WINDOW_US 1000000          /* health window */
#define CAL_NEEDED_WINDOWS 3u
#define VERIFY_MIN_OK 32u          /* unsaturated captures a verify needs */
#define BENCH_NFFT 64u             /* sdr_bench_moments_add */

_Static_assert(PSD_WS_BYTES <= SDR_BANK_PSD_BYTES, "psd workspace in the bank");
_Static_assert(SDR_RX_MAX_HOPS <= 8u, "iq_bad_mask is 8 bits");

/* Per-LO sums of the health window. */
typedef struct {
    float dc_i, dc_q;  /* sums of the captures' mean I/Q, counts */
    uint32_t caps, iq_off;
} lo_window_t;

/* Scan-task state (heap, sdr_rx_init). */
typedef struct {
    sdr_rx_config_t cfg;
    sdr_plan_cfg_t plan_cfg;
    sdr_plan_t plan;
    psd_iq_t iq[SDR_RX_MAX_HOPS]; /* per plan hop (one LO each) */
    unsigned nfft, ncap;
    unsigned hop;                 /* the next hop to measure; its LO is tuned_lo_khz */
    uint32_t tuned_lo_khz;        /* 0 = unknown: the next step tunes its hop's LO first */
    int64_t w_start, rot_start;
    uint32_t w_hops, w_rot;
    uint64_t w_tune, w_cap, w_wait, w_dsp, w_rot_us;
    uint32_t w_tune_max, w_dsp_max, w_rot_max;
    lo_window_t lo[SDR_RX_MAX_HOPS];
    uint8_t bad_windows;          /* consecutive windows with a DC or IQ problem */
    psd_result_t bench_psd;
} engine_t;
static engine_t *s_e;

/* Shared with any task. Counters: written by the scan task only (32-bit, no torn read). */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static sdr_rx_health_t s_health; /* window and plan snapshot, under s_mux */
static volatile uint32_t s_hops, s_rotations, s_timeouts, s_short, s_overrun, s_dsp_errors, s_stale;
static volatile bool s_init_running, s_recal_running, s_init_failed, s_cal_needed;
static volatile int s_req_recal = -1; /* a pending sdr_recal_t, or -1; under s_mux */
static volatile bool s_req_gain;
static volatile uint8_t s_gain_want = CONFIG_SDR_RX_GAIN_DEFAULT;
static volatile uint8_t s_gain_max = 83; /* the Kconfig range until the engine reads it */
static volatile sdr_plan_mode_t s_mode = SDR_PLAN_PAIRS;
static unsigned s_foreign_base;

esp_err_t sdr_rx_radio_start(void) {
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err; /* INVALID_STATE: it exists */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) return err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_NULL)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK) return err;
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) return err;
    if ((err = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)) != ESP_OK) return err;
#if !CONFIG_SDR_RX_PROMISC_FILTER_ALL
    /* No frame reaches the MAC's buffers or wakes the Wi-Fi task (C5VRX rf.c). */
    const wifi_promiscuous_filter_t filter = {.filter_mask = 0};
    if ((err = esp_wifi_set_promiscuous_filter(&filter)) != ESP_OK) return err;
#endif
    return ESP_OK;
}

sdr_rx_config_t sdr_rx_config_default(void) {
    static const uint16_t raceband_mhz[SDR_RX_CHANNELS] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
    sdr_rx_config_t c = {.mode = SDR_PLAN_PAIRS, .gain = CONFIG_SDR_RX_GAIN_DEFAULT};
    for (unsigned j = 0; j < SDR_RX_CHANNELS; j++) c.ch_khz[j] = raceband_mhz[j] * 1000u;
    return c;
}

static void window_restart(engine_t *e, int64_t now) {
    e->w_start = e->rot_start = now;
    e->w_hops = e->w_rot = 0;
    e->w_tune = e->w_cap = e->w_wait = e->w_dsp = e->w_rot_us = 0;
    e->w_tune_max = e->w_dsp_max = e->w_rot_max = 0;
    memset(e->lo, 0, sizeof(e->lo));
}

/* After a recalibration, a gain or plan change: the IQ states and the DC window start again
 * [W scanner.c:185]. */
static void dsp_reset(engine_t *e) {
    memset(e->iq, 0, sizeof(e->iq));
    window_restart(e, esp_timer_get_time());
}

static void publish_plan(engine_t *e) {
    portENTER_CRITICAL(&s_mux);
    s_health.nhops = e->plan.nhops;
    for (unsigned h = 0; h < SDR_RX_MAX_HOPS; h++)
        s_health.lo_mhz[h] = h < e->plan.nhops ? (uint16_t)(e->plan.hop[h].lo_khz / 1000u) : 0;
    portEXIT_CRITICAL(&s_mux);
    s_mode = e->cfg.mode;
}

static void plan_rebuild(engine_t *e, sdr_plan_mode_t mode) {
    e->cfg.mode = mode;
    sdr_plan_build(e->cfg.ch_khz, SDR_RX_CHANNELS, mode, &e->plan_cfg, &e->plan);
    sdr_plan_bands(&e->plan, e->nfft, RATE_HZ, BAND_HALF_HZ);
    e->hop = 0;
    e->tuned_lo_khz = 0;
    publish_plan(e);
}

static void count_capture_error(esp_err_t err) {
    if (err == ESP_ERR_TIMEOUT) s_timeouts++;
    else if (err == ESP_ERR_INVALID_SIZE) s_short++;
    else if (err == ESP_ERR_INVALID_RESPONSE) s_stale++;
    else s_overrun++; /* ESP_ERR_INVALID_STATE: guard words changed (or shut down) */
}

static IRAM_ATTR esp_err_t tune(engine_t *e, uint32_t lo_khz) {
    esp_err_t err = radio_tune(lo_khz);
    e->tuned_lo_khz = err == ESP_OK ? lo_khz : 0;
    return err;
}

/* max(|mean I|, |mean Q|) of a capture, counts. */
static uint32_t capture_dc(const uint32_t *w, unsigned n) {
    int32_t si = 0, sq = 0;
    for (unsigned j = 0; j < n; j++) {
        si += radio_word_i(w[j]);
        sq += radio_word_q(w[j]);
    }
    const uint32_t a = (uint32_t)abs(si), b = (uint32_t)abs(sq);
    return ((a > b ? a : b) + n / 2) / n;
}

/* The FULL verify of one freshly calibrated LO (spec 2.8): at the verify gain, where the
 * receiver's noise is over the ADC's, plain I/Q moment sums of VERIFY_CAPTURES captures give
 * the raw path's image rejection; under IRR_MIN_DB, that LO is calibrated again (at most
 * VERIFY_TRIES calibrations). Returns false when no capture worked at all. */
static bool verify_lo(engine_t *e, unsigned h, uint32_t cal_ms) {
    const uint32_t lo = e->plan.hop[h].lo_khz;
    unsigned tries = 1, ok = 0, sat = 0, errors = 0;
    uint32_t dc = 0;
    float irr = 0.0f;
    for (;;) {
        sdr_moments_t m = {0};
        ok = sat = errors = 0;
        dc = 0;
        (void)radio_gain_now(CONFIG_SDR_RX_VERIFY_GAIN);
        for (unsigned c = 0; c < CONFIG_SDR_RX_VERIFY_CAPTURES; c++) {
            const uint32_t *w;
            const esp_err_t err = radio_capture(e->ncap, RATE_CODE, CONFIG_SDR_RX_SETTLE_US, &w, NULL, NULL, NULL);
            if (err == ESP_ERR_INVALID_RESPONSE) {
                /* PHY tracking recalibrated meanwhile: this LO again, at the verify gain. */
                s_stale++;
                (void)tune(e, lo);
                (void)radio_gain_now(CONFIG_SDR_RX_VERIFY_GAIN);
                continue;
            }
            if (err != ESP_OK) {
                count_capture_error(err);
                errors++;
                continue;
            }
            if (psd_moments_add(w, e->ncap, e->nfft, &m)) {
                sat++;
                continue;
            }
            ok++;
            const uint32_t d = capture_dc(w, e->ncap);
            if (d > dc) dc = d;
        }
        (void)radio_gain_now(radio_gain()); /* the scan gain again */
        if (ok < VERIFY_MIN_OK) break;
        const psd_iq_t mean = psd_moments_mean(&m);
        irr = psd_iq_irr_db(&mean);
        if (irr >= CONFIG_SDR_RX_IRR_MIN_DB || tries >= CONFIG_SDR_RX_VERIFY_TRIES) break;
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t re = radio_recal_lo(lo); /* forget its cache slot, calibrate it again */
        e->tuned_lo_khz = re == ESP_OK ? lo : 0;
        if (re != ESP_OK) break;
        cal_ms += (uint32_t)((esp_timer_get_time() - t0) / 1000);
        tries++;
    }
    if (ok < VERIFY_MIN_OK)
        printf("SDR cal lo=%" PRIu32 " ms=%" PRIu32 " irr_db=%s verify_gain=%u tries=%u dc=%" PRIu32
               " sat=%u errors=%u\n",
               lo / 1000u, cal_ms, sat >= errors ? "skip_saturated" : "skip_faults",
               (unsigned)CONFIG_SDR_RX_VERIFY_GAIN, tries, dc, sat, errors);
    else
        printf("SDR cal lo=%" PRIu32 " ms=%" PRIu32 " irr_db=%.1f verify_gain=%u tries=%u dc=%" PRIu32 " sat=%u\n",
               lo / 1000u, cal_ms, (double)irr, (unsigned)CONFIG_SDR_RX_VERIFY_GAIN, tries, dc, sat);
    return ok || sat;
}

/* Every plan LO recalibrated (spec 2.8): the cache cleared once (never per LO: that would leave
 * only the last LO cached), then one tune per LO, which misses the cache, calibrates (~41 ms)
 * and saves; FULL verifies each. One tick between LOs, so the link and IDLE run. */
static esp_err_t recal_all(engine_t *e, sdr_recal_t kind, bool *captured) {
    esp_err_t err = ESP_OK;
    bool all_captured = true;
    radio_force_recal();
    for (unsigned h = 0; h < e->plan.nhops; h++) {
        if (h) vTaskDelay(1);
        const uint32_t lo = e->plan.hop[h].lo_khz;
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t te = tune(e, lo);
        const uint32_t cal_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (te != ESP_OK) {
            printf("SDR cal lo=%" PRIu32 " err=%s\n", lo / 1000u, esp_err_to_name(te));
            err = te;
            continue;
        }
        if (kind == SDR_RECAL_FULL) all_captured &= verify_lo(e, h, cal_ms);
        else printf("SDR cal lo=%" PRIu32 " ms=%" PRIu32 " kind=quick\n", lo / 1000u, cal_ms);
    }
    dsp_reset(e);
    if (e->plan.nhops) {
        const esp_err_t te = tune(e, e->plan.hop[0].lo_khz); /* a cache restore, no calibration */
        if (te != ESP_OK && err == ESP_OK) err = te;
    }
    e->hop = 0;
    e->bad_windows = 0;
    if (err == ESP_OK) s_cal_needed = false;
    window_restart(e, esp_timer_get_time());
    if (captured) *captured = all_captured;
    return err;
}

static void apply_recal(engine_t *e, sdr_hop_t *out) {
    portENTER_CRITICAL(&s_mux);
    const int kind = s_req_recal;
    s_req_recal = -1;
    s_recal_running = kind >= 0;
    portEXIT_CRITICAL(&s_mux);
    if (kind < 0) return;
    (void)recal_all(e, (sdr_recal_t)kind, NULL);
    out->recalibrated = true;
    s_recal_running = false;
}

static void apply_gain(engine_t *e) {
    s_req_gain = false;
    const uint8_t gain = s_gain_want;
    radio_set_gain(gain);
    (void)radio_gain_now(gain);
    dsp_reset(e);
}

static esp_err_t init_engine(const sdr_rx_config_t *cfg) {
    if (s_e) return ESP_ERR_INVALID_STATE;
    engine_t *e = heap_caps_calloc(1, sizeof(*e), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!e) return ESP_ERR_NO_MEM;
    e->cfg = cfg ? *cfg : sdr_rx_config_default();
    e->nfft = CONFIG_SDR_RX_NFFT;
    if (e->nfft != 64u && e->nfft != 128u && e->nfft != 256u) {
        printf("SDR nfft=%u refused (64, 128 or 256): 64\n", e->nfft);
        e->nfft = 64u;
    }
    e->ncap = SEGMENTS * e->nfft;
    e->plan_cfg = sdr_plan_cfg_default();
    s_e = e;
    /* radio_init releases the bank: nothing below touches it before. */
    esp_err_t err = radio_init();
    if (err != ESP_OK) return err;
    size_t ws_bytes = 0;
    void *ws = radio_dsp_region(&ws_bytes);
    if ((err = psd_init(ws, ws_bytes)) != ESP_OK) return err;
    metrics_init();
    radio_set_filter(CONFIG_SDR_RX_FILTER_CODE);
    s_gain_want = e->cfg.gain;
    radio_set_gain(e->cfg.gain);
    plan_rebuild(e, e->cfg.mode);
    if (!e->plan.nhops) return ESP_ERR_INVALID_ARG;
    s_foreign_base = radio_foreign_dc();
    bool captured = true;
    if ((err = recal_all(e, SDR_RECAL_FULL, &captured)) != ESP_OK) return err;
    esp_rom_delay_us(3000); /* as widscan58's radio_init after its first tune */
    s_gain_max = (uint8_t)radio_gain_max();
    if (!captured) return ESP_ERR_TIMEOUT; /* an LO gave no capture at all: the dump does not run */
    window_restart(e, esp_timer_get_time());
    return ESP_OK;
}

esp_err_t sdr_rx_init(const sdr_rx_config_t *cfg) {
    s_init_running = true;
    const esp_err_t err = init_engine(cfg);
    if (err != ESP_OK) {
        s_init_failed = true;
        s_cal_needed = true;
        printf("SDR init error err=%s\n", esp_err_to_name(err));
    }
    s_init_running = false;
    return err;
}

/* Once a second, at a rotation end. Out of IRAM: it is not on the hop's path. */
static __attribute__((noinline)) void window_close(engine_t *e, int64_t now) {
    const uint32_t hops = e->w_hops ? e->w_hops : 1u, rots = e->w_rot ? e->w_rot : 1u;
    const int64_t span = now - e->w_start;
    uint32_t dc_max = 0;
    uint8_t iq_bad = 0;
    for (unsigned h = 0; h < e->plan.nhops; h++) {
        const lo_window_t *w = &e->lo[h];
        if (!w->caps) continue;
        const float mi = fabsf(w->dc_i / (float)w->caps), mq = fabsf(w->dc_q / (float)w->caps);
        const uint32_t dc = (uint32_t)lrintf(mi > mq ? mi : mq);
        if (dc > dc_max) dc_max = dc;
        if (2u * w->iq_off > w->caps) iq_bad |= (uint8_t)(1u << h);
    }
    const bool bad = dc_max > CONFIG_SDR_RX_DC_CAL_NEEDED || iq_bad;
    e->bad_windows = bad ? (uint8_t)(e->bad_windows < 255u ? e->bad_windows + 1u : 255u) : 0u;
    if (e->bad_windows >= CAL_NEEDED_WINDOWS) s_cal_needed = true; /* until a recalibration */
    const uint8_t gain_max = (uint8_t)radio_gain_max();
    s_gain_max = gain_max;
    portENTER_CRITICAL(&s_mux);
    sdr_rx_health_t *o = &s_health;
    o->rot_hz_x10 = span > 0 ? (uint16_t)(((uint64_t)e->w_rot * 10000000u + (uint64_t)span / 2) / (uint64_t)span) : 0;
    o->rot_us_avg = (uint32_t)(e->w_rot_us / rots);
    o->rot_us_max = e->w_rot_max;
    o->tune_us_avg = (uint32_t)(e->w_tune / hops);
    o->tune_us_max = e->w_tune_max;
    o->cap_us_avg = (uint32_t)(e->w_cap / hops);
    o->wait_us_avg = (uint32_t)(e->w_wait / hops);
    o->dsp_us_avg = (uint32_t)(e->w_dsp / hops);
    o->dsp_us_max = e->w_dsp_max;
    o->dc_max = (uint16_t)(dc_max > 65535u ? 65535u : dc_max);
    o->iq_bad_mask = iq_bad;
    o->gain_max = gain_max;
    portEXIT_CRITICAL(&s_mux);
    const int64_t rot_start = e->rot_start;
    window_restart(e, now);
    e->rot_start = rot_start;
}

IRAM_ATTR void sdr_rx_step(sdr_hop_t *out) {
    memset(out, 0, sizeof(*out));
    engine_t *e = s_e;
    if (!e || s_init_failed || !e->plan.nhops) return;
    if (s_req_recal >= 0) apply_recal(e, out);
    if (s_req_gain) apply_gain(e);
    const unsigned h = e->hop;
    const sdr_plan_hop_t *hop = &e->plan.hop[h];
    out->hop = (uint8_t)h;
    const int64_t t0 = esp_timer_get_time();
    /* Normally the previous step already tuned this LO (below); after a bench or a failed tune
     * it is tuned here. */
    if (e->tuned_lo_khz != hop->lo_khz) (void)tune(e, hop->lo_khz);
    const int64_t t1 = esp_timer_get_time();
    const uint32_t recals = radio_recals();
    const uint32_t *words = NULL;
    int64_t t_mid = 0;
    uint32_t waited = 0;
    /* Waits out whatever is left of the settle time. */
    const esp_err_t err = radio_capture(e->ncap, RATE_CODE, CONFIG_SDR_RX_SETTLE_US, &words, &t_mid, NULL, &waited);
    const int64_t t2 = esp_timer_get_time();
    out->t.cap_us = (uint32_t)(t2 - t1);
    out->t.wait_us = waited;
    if (err == ESP_ERR_INVALID_RESPONSE) {
        /* Stale (spec 2.1): PHY tracking recalibrated since this LO's tune, which left it
         * unforced and on the PHY's ADC path. Re-tune it, which recalibrates it (the tracker's
         * tables replaced the cache); the next step captures it again. */
        s_stale++;
        out->stale = true;
        (void)tune(e, hop->lo_khz);
        out->t.tune_us = (uint32_t)(t1 - t0 + esp_timer_get_time() - t2);
        out->recalibrated = radio_recals() != recals;
        return;
    }
    if (err != ESP_OK) count_capture_error(err);
    /* Tune the next hop's LO now: it settles while this hop's DSP runs. */
    const unsigned next = h + 1u < e->plan.nhops ? h + 1u : 0u;
    (void)tune(e, e->plan.hop[next].lo_khz);
    const int64_t t3 = esp_timer_get_time();
    out->recalibrated |= radio_recals() != recals;
    if (err == ESP_OK) {
        psd_result_t psd;
        const esp_err_t de = psd_compute(words, e->ncap, e->nfft, SEGMENTS, (float)RATE_HZ, CONJUGATE, USABLE_HZ,
                                         DC_GUARD, &e->iq[h], &psd);
        if (de != ESP_OK) {
            s_dsp_errors++;
        } else {
            const int32_t scale_q8 = sdr_level_scale_q8(&psd);
            unsigned n = 0;
            for (unsigned j = 0; j < hop->nch && n < SDR_RX_PER_HOP; j++, n++) {
                sdr_reading_t *r = &out->r[n];
                r->t_us = t_mid;
                r->channel = hop->ch[j];
                r->level_qdb = sdr_band_level_qdb(&psd, hop->band[j], scale_q8, &r->flags);
            }
            out->n = (uint8_t)n;
            lo_window_t *w = &e->lo[h];
            w->dc_i += psd.dc_i;
            w->dc_q += psd.dc_q;
            w->caps++;
            if (!psd.iq_applied) w->iq_off++;
        }
    }
    const int64_t t4 = esp_timer_get_time();
    out->t.tune_us = (uint32_t)(t1 - t0 + t3 - t2);
    out->t.dsp_us = (uint32_t)(t4 - t3);
    e->hop = next;
    s_hops++;
    e->w_hops++;
    e->w_tune += out->t.tune_us;
    e->w_cap += out->t.cap_us;
    e->w_wait += out->t.wait_us;
    e->w_dsp += out->t.dsp_us;
    if (out->t.tune_us > e->w_tune_max) e->w_tune_max = out->t.tune_us;
    if (out->t.dsp_us > e->w_dsp_max) e->w_dsp_max = out->t.dsp_us;
    if (next == 0) {
        out->rotation_end = true;
        s_rotations++;
        const uint32_t rot_us = (uint32_t)(t4 - e->rot_start);
        e->rot_start = t4;
        e->w_rot++;
        e->w_rot_us += rot_us;
        if (rot_us > e->w_rot_max) e->w_rot_max = rot_us;
        if (t4 - e->w_start >= WINDOW_US) window_close(e, t4);
    }
}

bool sdr_rx_request_recal(sdr_recal_t kind) {
    if (kind != SDR_RECAL_QUICK && kind != SDR_RECAL_FULL) return false;
    portENTER_CRITICAL(&s_mux);
    const bool ok = s_req_recal < 0 && !s_recal_running && !s_init_running;
    if (ok) s_req_recal = (int)kind;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void sdr_rx_request_gain(uint8_t gain) {
    s_gain_want = gain;
    s_req_gain = true;
}

bool sdr_rx_calibrating(void) {
    return s_init_running || s_recal_running || s_req_recal >= 0 || radio_calibrating();
}

uint8_t sdr_rx_gain(void) { return s_gain_want; }
uint8_t sdr_rx_gain_max(void) { return s_gain_max; }

uint32_t sdr_rx_fault_count(void) {
    return s_timeouts + s_short + s_overrun + s_dsp_errors + (s_init_failed ? 1u : 0u);
}

void sdr_rx_get_health(sdr_rx_health_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_health;
    portEXIT_CRITICAL(&s_mux);
    out->hops = s_hops;
    out->rotations = s_rotations;
    out->cap_timeouts = s_timeouts;
    out->cap_short = s_short;
    out->cap_overrun = s_overrun;
    out->dsp_errors = s_dsp_errors;
    out->stale_caps = s_stale;
    out->recals = radio_recals();
    out->foreign_dc = radio_foreign_dc() - s_foreign_base;
    out->gain = s_gain_want;
    out->gain_max = s_gain_max;
    out->calibrating = sdr_rx_calibrating();
    out->init_failed = s_init_failed;
    out->cal_needed = s_cal_needed || s_init_failed;
}

const char *sdr_rx_phy_version(void) { return radio_phy_version(); }

void sdr_rx_print(void) {
    sdr_rx_health_t h;
    sdr_rx_get_health(&h);
    char lo[8 * 6 + 1] = "";
    size_t len = 0;
    for (unsigned j = 0; j < h.nhops && j < SDR_RX_MAX_HOPS && len + 6 < sizeof(lo); j++)
        len += (size_t)snprintf(lo + len, sizeof(lo) - len, "%s%u", j ? "," : "", (unsigned)h.lo_mhz[j]);
    printf("SDR plan=%s lo=%s gain=%u/%u rot_hz_x10=%u rot_us=%" PRIu32 "/%" PRIu32 " tune_us=%" PRIu32 "/%" PRIu32
           " cap_us=%" PRIu32 " wait_us=%" PRIu32 " dsp_us=%" PRIu32 "/%" PRIu32 " faults=%" PRIu32
           " timeout=%" PRIu32 " short=%" PRIu32 " overrun=%" PRIu32 " dsp=%" PRIu32 " stale=%" PRIu32
           " recal=%" PRIu32 " foreign_dc=%" PRIu32 " dc_max=%u iq_bad=0x%x cal_needed=%u calibrating=%u"
           " init_failed=%u phy=%s\n",
           s_mode == SDR_PLAN_SINGLE ? "single" : "pairs", lo, (unsigned)h.gain, (unsigned)h.gain_max,
           (unsigned)h.rot_hz_x10, h.rot_us_avg, h.rot_us_max, h.tune_us_avg, h.tune_us_max, h.cap_us_avg,
           h.wait_us_avg, h.dsp_us_avg, h.dsp_us_max, sdr_rx_fault_count(), h.cap_timeouts, h.cap_short,
           h.cap_overrun, h.dsp_errors, h.stale_caps, h.recals, h.foreign_dc, (unsigned)h.dc_max,
           (unsigned)h.iq_bad_mask, h.cal_needed, h.calibrating, h.init_failed, sdr_rx_phy_version());
}

/* ---- bench (sdr_rx_bench.h): scan task only, between steps ---- */

void sdr_bench_set_tune_method(sdr_tune_method_t m) { radio_set_full_tune(m == SDR_TUNE_FULL); }

esp_err_t sdr_bench_tune(uint32_t lo_khz, uint32_t *tune_us) {
    if (!s_e) return ESP_ERR_INVALID_STATE;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = tune(s_e, lo_khz);
    if (tune_us) *tune_us = (uint32_t)(esp_timer_get_time() - t0);
    return err;
}

esp_err_t sdr_bench_capture(unsigned n, uint32_t settle_us, const uint32_t **words, int64_t *t_mid_us,
                            uint32_t *cap_us) {
    if (!s_e) return ESP_ERR_INVALID_STATE;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = radio_capture(n, RATE_CODE, settle_us, words, t_mid_us, NULL, NULL);
    if (cap_us) *cap_us = (uint32_t)(esp_timer_get_time() - t0);
    if (err == ESP_ERR_INVALID_RESPONSE) s_e->tuned_lo_khz = 0; /* the next tune recalibrates */
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) count_capture_error(err);
    return err;
}

esp_err_t sdr_bench_psd(const uint32_t *words, unsigned n, unsigned nfft, int iq_lo, sdr_psd_view_t *out) {
    engine_t *e = s_e;
    if (!e) return ESP_ERR_INVALID_STATE;
    psd_iq_t *iq = iq_lo >= 0 && (unsigned)iq_lo < e->plan.nhops ? &e->iq[iq_lo] : NULL;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = psd_compute(words, n, nfft, PSD_MAX_SEGMENTS, (float)RATE_HZ, CONJUGATE, USABLE_HZ,
                                      DC_GUARD, iq, &e->bench_psd);
    if (err != ESP_OK) return err;
    const psd_result_t *p = &e->bench_psd;
    *out = (sdr_psd_view_t){.p = p->p, .valid = p->valid, .scale = p->scale, .enbw_bins = p->enbw_bins,
                            .dc_i = p->dc_i, .dc_q = p->dc_q, .nfft = p->nfft, .segments = p->segments,
                            .saturated = p->saturated, .iq_applied = p->iq_applied,
                            .us = (uint32_t)(esp_timer_get_time() - t0)};
    return ESP_OK;
}

int16_t sdr_bench_level(const sdr_psd_view_t *v, int32_t offset_hz, uint32_t half_hz, uint8_t *flags) {
    const psd_result_t p = {.nfft = v->nfft, .segments = v->segments, .enbw_bins = v->enbw_bins,
                            .saturated = v->saturated, .p = (uint32_t *)v->p, .scale = v->scale,
                            .valid = (uint8_t *)v->valid};
    return sdr_band_level_qdb(&p, sdr_plan_band(v->nfft, RATE_HZ, offset_hz, half_hz), sdr_level_scale_q8(&p), flags);
}

float sdr_bench_irr_db(int lo) {
    if (!s_e || lo < 0 || (unsigned)lo >= s_e->plan.nhops) return 0.0f;
    return psd_iq_irr_db(&s_e->iq[lo]);
}

unsigned sdr_bench_moments_add(const uint32_t *words, unsigned n, sdr_moments_t *m) {
    return psd_moments_add(words, n, BENCH_NFFT, m);
}

float sdr_bench_moments_irr_db(const sdr_moments_t *m) {
    const psd_iq_t mean = psd_moments_mean(m);
    return psd_iq_irr_db(&mean);
}

esp_err_t sdr_bench_borrow_cal(uint32_t lo_khz, uint32_t cal_from_khz) {
    if (!s_e) return ESP_ERR_INVALID_STATE;
    s_e->tuned_lo_khz = 0;
    return radio_borrow_cal(lo_khz, cal_from_khz);
}

void sdr_bench_borrow_end(void) {
    if (!s_e) return;
    radio_borrow_end();
    s_e->tuned_lo_khz = 0;
    dsp_reset(s_e);
}

const sdr_plan_t *sdr_bench_plan(void) { return s_e ? &s_e->plan : NULL; }

void sdr_bench_set_plan(sdr_plan_mode_t m) {
    if (!s_e) return;
    plan_rebuild(s_e, m);
    dsp_reset(s_e);
}

void *sdr_bench_scratch(size_t *bytes) { return radio_bench_region(bytes); }
