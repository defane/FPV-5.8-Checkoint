/* The console's engine benches (cp_bench.h; docs/bench.md "Engine bench"), K then:
 *   s  timing: the rotation rate and every step of a hop, PLL-only tune then full channel setup
 *   z  settle/residue: what a strong LO leaves in the next LO's capture, against a quiet control
 *   x  image rejection between the two channels of an LO: a floor run (VTX off), then VTX on
 *   n  noise of a reading against the capture size, and a borrowed-calibration DC control
 *   p  the SINGLE plan (one LO per channel) against PAIRS, for 1 s
 *   S  scan recording: every channel's level and every LO's saturated captures over the last
 *      >= 60 s, dumped when stopped
 * The methods are widscan58's (CapnBry, https://github.com/CapnBry/widscan58, a9fff97, PLAN.md:
 * tune timing, settle, image rejection and noise per FFT size); the recording and the residue
 * trace keep the shape of the fork's C5VRX-era hop bench (hop_bench.c before issue #2).
 *
 * Each runs inside the scan task (cp_scan.c), between two sdr_rx_step calls, and leaves the
 * engine to it: the scan task then restores the production settings (PAIRS plan, PLL tune, IQ
 * states reset) and relearns the rests. No detection meanwhile. The buffers live in the RF dump
 * bank's bench scratch (sdr_bench_scratch, 44 KiB: scan task only, never during a capture), none
 * in .bss. Every loop calls cp_yield(), so IDLE runs and the task watchdog is fed. Printing goes
 * through the non-blocking stdout, paced (5 ms every 4 lines). */
#include "cp_bench.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "checkpoint.h"
#include "hop_stats.h"
#include "sdkconfig.h"
#include "sdr_rx_bench.h"

#define SCRATCH_BYTES   (44u * 1024u)  /* sdr_bench_scratch() (spec 2.3) */
#define NFFT            64u            /* every bench PSD: 64 points at 80 MS/s, as production */
#define BAND_HALF_HZ    5000000u       /* a channel's band: +-5 MHz, as production */
#define CAP_WORDS       256u           /* the production capture (4 x 64) */
#define ROT_STEPS_MAX   (4u * SDR_RX_MAX_HOPS)   /* a rotation's steps, stale retries included */
#define ROT_READINGS    (SDR_RX_MAX_HOPS * SDR_RX_PER_HOP)
#define PACE_LINES      4u
#define PACE_MS         5u

/* K s */
#define S_PLL_ROTATIONS  2000u
#define S_FULL_ROTATIONS 500u
#define S_HIST_BINS      1024u         /* 1 us bins: every per-hop median is far below (a ~41 ms
                                        * calibration counts in the last bin, its max stays exact) */
/* K z, x, p */
#define LV_ROTATIONS     200u          /* rotations of medians */
#define Z_REPEATS        8u
#define Z_STRONG_US      3000u         /* on the strong LO before the hop */
#define Z_WORDS          4096u         /* 51.2 us */
#define Z_QUARTERS       4u
#define Z_TOL_QDB        4             /* 1 dB */
#define X_CAPTURES       200u
#define X_SIGNAL_QDB     40            /* 10 dB over the floor: a VTX is on, so not a floor run */
/* A floor run with a channel over 6 dB above the median of the 8 is refused: a VTX is on. With
 * every VTX off the 8 floors spread 1.25 dB at gain 40, under 1 dB from their median ([board] K n,
 * 2026-11-15: -239..-234 qdb), and each is a median of 200 readings of ~1.06 dB sd ([board] B4),
 * so ~0.1 dB of noise: 6 dB leaves ~5 dB for another board, antenna or gain. A channel at
 * X_SIGNAL_QDB over its own floor stands 9 dB or more over that median, so no VTX that would
 * count as a signal is stored as a floor. Floor runs taken with a 25 mW VTX on R6 had R6 49-57 dB
 * over the median ([board] 2026-11-15). The median ignores up to 3 raised channels of 8, so a
 * strong VTX's partner (R5 at +3.75 dB in those runs) does not hide it. */
#define X_FLOOR_SPREAD_QDB 24
#define X_NEED_QDB       200           /* 50 dB over the floor: the image is measurable */
#define X_FLOOR_DB       0.5f          /* the partner this close to its floor: floor-limited */
#define X_FLOOR_EXCESS_DB 9.1f         /* hs_excess_db at X_FLOOR_DB: 9.1 dB under the floor */
#define P_SINGLE_ROTATIONS 100u
/* K n */
#define N_CAPTURES       500u          /* per plan LO and capture size */
#define N_BORROW_CAPTURES 200u
/* K S */
#define R_KEEP_S         60u
#define R_MAX_S          3600u
#define R_LIVE_S         10u
#define R_NOMINAL_HZ_X10 5100u         /* before the engine's first 1 s window (never in practice) */
#define R_DUMP_PAUSE_MS  200u
#define R_SAT_HOPS       4u            /* a row's saturated-capture counts: PAIRS's 4 LOs, 4 bits each */
#define R_SAT_MAX        15u           /* a count over 15 (decim over 15) reads 15 */

static const uint16_t k_z_delay_us[] = {0, 25, 50, 100, 150, 200, 300, 500, 800, 1000, 2000};
#define Z_DELAYS (sizeof k_z_delay_us / sizeof k_z_delay_us[0])
static const uint16_t k_n_words[] = {256, 1024, 4096};
#define N_SIZES (sizeof k_n_words / sizeof k_n_words[0])

enum { T_TUNE, T_CAP, T_WAIT, T_DSP, T_DETECTOR, T_COUNT };

/* The scratch, per bench. */
typedef struct {
    uint32_t bins[T_COUNT][S_HIST_BINS];
    uint32_t rot_us[S_PLL_ROTATIONS];
} s_bench_t;

typedef struct {
    int32_t lv[SDR_RX_CHANNELS][LV_ROTATIONS];
    uint32_t rot_us[LV_ROTATIONS];
} lv_buf_t;

typedef struct {
    lv_buf_t lv;
    int32_t sum[2][Z_DELAYS][Z_QUARTERS];          /* after, control */
    uint16_t n[2][Z_DELAYS];
    int16_t mean[2][Z_DELAYS * Z_QUARTERS];
} z_bench_t;

typedef struct {
    lv_buf_t lv;
    int32_t s_on[X_CAPTURES], p_on[X_CAPTURES], p_off[X_CAPTURES], p_single[X_CAPTURES];
} x_bench_t;

typedef struct {
    int16_t lv[SDR_RX_CHANNELS][N_CAPTURES];
    uint32_t dsp_us[SDR_RX_MAX_HOPS * N_CAPTURES];
} n_bench_t;

/* Packed: 22 bytes, so the 44 KiB scratch holds 2048 rows, decim 15 at the board's 507.9 rotations
 * a second ([board] B1); aligned, 24 bytes would hold 1877 rows, decim 17. */
typedef struct __attribute__((packed)) {
    uint32_t t_us;                                 /* the row's first rotation start, from the recording's */
    int16_t qdb[SDR_RX_CHANNELS];                  /* mean of decim rotations; SDR_LEVEL_NONE */
    uint16_t sat;                                  /* saturated captures per hop among those rotations,
                                                    * 4 bits a hop, hop 0 in the top nibble; <= R_SAT_MAX */
} scan_row_t;

_Static_assert(sizeof(s_bench_t) <= SCRATCH_BYTES && sizeof(z_bench_t) <= SCRATCH_BYTES &&
               sizeof(x_bench_t) <= SCRATCH_BYTES && sizeof(n_bench_t) <= SCRATCH_BYTES, "bench scratch");
_Static_assert(sizeof(scan_row_t) == 22u && SCRATCH_BYTES / sizeof(scan_row_t) == 2048u,
               "22-byte recording rows: 2048 in the scratch");
_Static_assert(R_SAT_HOPS * 4u <= 16u, "a row's saturated counts fit its 16 bits");
_Static_assert(SDR_RX_MAX_HOPS <= 8u, "rotation_t.sat_hops is 8 bits");
_Static_assert(ROT_READINGS >= SDR_RX_CHANNELS, "a rotation reads every channel once");

typedef struct {
    sdr_reading_t r[ROT_READINGS];
    unsigned n, stale;
    uint8_t sat_hops;                              /* a bit per plan hop whose capture hit an ADC rail */
} rotation_t;

/* ---- helpers ---- */

static void pace(cp_yield_t *y, unsigned *lines)
{
    if (++*lines % PACE_LINES == 0u) vTaskDelay(pdMS_TO_TICKS(PACE_MS));
    cp_yield(y);
}

static int32_t div_round(int32_t s, int32_t n) { return s >= 0 ? (s + n / 2) / n : -((-s + n / 2) / n); }

/* "a/b/..." of n values; a missing one (have[i] false) prints "none". */
static const char *fmt_list(char *buf, size_t size, const int32_t *v, const bool *have, unsigned n)
{
    size_t len = 0;
    buf[0] = '\0';
    for (unsigned i = 0; i < n && len < size; i++) {
        const char *sep = i ? "/" : "";
        const int w = have && !have[i] ? snprintf(buf + len, size - len, "%snone", sep)
                                       : snprintf(buf + len, size - len, "%s%" PRId32, sep, v[i]);
        if (w < 0) break;
        len += (size_t)w;
    }
    return buf;
}

/* The plan hop that reads channel ch (and its slot j in it), or -1. */
static int hop_of(const sdr_plan_t *p, unsigned ch, unsigned *j)
{
    for (unsigned h = 0; h < p->nhops; h++)
        for (unsigned k = 0; k < p->hop[h].nch; k++)
            if (p->hop[h].ch[k] == ch) {
                if (j) *j = k;
                return (int)h;
            }
    return -1;
}

/* Production steps up to the next rotation end, its readings in o. t (K s): the per-hop
 * timings and, with feed, the detector's time per reading; feed: the readings also go to that
 * detector, as in production. False when no rotation ended within ROT_STEPS_MAX steps. A yield
 * before a step falls in this rotation. */
static bool next_rotation(cp_yield_t *y, rotation_t *o, hs_hist_t *t, scan_detector_t *feed)
{
    o->n = 0;
    o->stale = 0;
    o->sat_hops = 0;
    for (unsigned k = 0; k < ROT_STEPS_MAX; k++) {
        cp_yield(y);
        sdr_hop_t h;
        sdr_rx_step(&h);
        if (h.stale) {
            o->stale++;                            /* no capture: not a hop's timing */
        } else if (t) {
            hs_hist_add(&t[T_TUNE], h.t.tune_us);
            hs_hist_add(&t[T_CAP], h.t.cap_us);
            hs_hist_add(&t[T_WAIT], h.t.wait_us);
            if (h.n) hs_hist_add(&t[T_DSP], h.t.dsp_us);
        }
        /* One flag per capture, on both of its readings. */
        if (h.n && (h.r[0].flags & SDR_RD_SATURATED) && h.hop < SDR_RX_MAX_HOPS) o->sat_hops |= (uint8_t)(1u << h.hop);
        if (feed && h.n) {                         /* as production feeds it; the time per reading */
            pd_event_t ev[SDR_RX_PER_HOP];
            unsigned held;
            const int64_t t0 = esp_timer_get_time();
            (void)cp_feed_hop(feed, &h, ev, &held);
            if (t) hs_hist_add(&t[T_DETECTOR], (uint32_t)((esp_timer_get_time() - t0) / h.n));
        }
        for (unsigned i = 0; i < h.n; i++)
            if (o->n < ROT_READINGS) o->r[o->n++] = h.r[i];
        if (h.rotation_end) {
            if (feed) sd_rotation(feed, esp_timer_get_time());
            return true;
        }
    }
    return false;
}

/* Every channel's median level over `rotations` production rotations (IQ correction on, as in
 * production), after a whole first one, and the median time of the rotations without a yield.
 * Returns the rotations done. */
static unsigned median_levels(cp_yield_t *y, lv_buf_t *b, unsigned rotations, int32_t med[SDR_RX_CHANNELS],
                              bool have[SDR_RX_CHANNELS], uint32_t *rot_med_us)
{
    unsigned n[SDR_RX_CHANNELS] = {0}, nrot = 0, done = 0;
    rotation_t r;
    if (rotations > LV_ROTATIONS) rotations = LV_ROTATIONS;
    if (next_rotation(y, &r, NULL, NULL)) {
        int64_t prev = esp_timer_get_time();
        for (; done < rotations; done++) {
            const uint32_t y0 = y->count;
            if (!next_rotation(y, &r, NULL, NULL)) break;
            const int64_t now = esp_timer_get_time();
            if (y->count == y0) b->rot_us[nrot++] = (uint32_t)(now - prev);
            prev = now;
            for (unsigned i = 0; i < r.n; i++) {
                const unsigned ch = r.r[i].channel;
                if (ch < SDR_RX_CHANNELS && r.r[i].level_qdb != SDR_LEVEL_NONE && n[ch] < LV_ROTATIONS)
                    b->lv[ch][n[ch]++] = r.r[i].level_qdb;
            }
        }
    }
    for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) {
        have[ch] = n[ch] > 0u;
        med[ch] = have[ch] ? hs_spread_i32(b->lv[ch], n[ch]).median : SDR_LEVEL_NONE;
    }
    if (rot_med_us) *rot_med_us = hs_spread(b->rot_us, nrot).med;
    return done;
}

static uint32_t hz_x10(uint32_t rot_us) { return rot_us ? (10000000u + rot_us / 2u) / rot_us : 0u; }

/* One capture of n words at lo (tuned by the caller), the settle wait as production. A stale one
 * (PHY tracking recalibrated since the tune) re-tunes lo, which recalibrates it, and tries again. */
static esp_err_t capture_at(uint32_t lo_khz, unsigned n, const uint32_t **w)
{
    esp_err_t err = sdr_bench_capture(n, CONFIG_SDR_RX_SETTLE_US, w, NULL, NULL);
    if (err == ESP_ERR_INVALID_RESPONSE && sdr_bench_tune(lo_khz, NULL) == ESP_OK)
        err = sdr_bench_capture(n, CONFIG_SDR_RX_SETTLE_US, w, NULL, NULL);
    return err;
}

/* ---- K s: rotation rate and per-step timing ---- */

static void timing_run(cp_t *cp, cp_yield_t *y, s_bench_t *b, sdr_tune_method_t method, unsigned rotations,
                       unsigned *lines)
{
    hs_hist_t t[T_COUNT];
    for (unsigned k = 0; k < T_COUNT; k++) hs_hist_init(&t[k], b->bins[k], S_HIST_BINS);
    sdr_bench_set_tune_method(method);
    rotation_t r;
    unsigned done = 0, nrot = 0, yields = 0, stale = 0;
    const uint32_t faults0 = sdr_rx_fault_count();
    if (next_rotation(y, &r, NULL, NULL)) {        /* the first one may have started mid-way */
        int64_t prev = esp_timer_get_time();
        for (; done < rotations; done++) {
            const uint32_t y0 = y->count;
            if (!next_rotation(y, &r, t, &cp->sd)) break;
            const int64_t now = esp_timer_get_time();
            stale += r.stale;
            if (y->count == y0) b->rot_us[nrot++] = (uint32_t)(now - prev);
            else yields++;                         /* a blocked tick is not the engine's time */
            prev = now;
        }
    }
    const hs_spread_t rot = hs_spread(b->rot_us, nrot), tu = hs_hist_spread(&t[T_TUNE]),
                      ca = hs_hist_spread(&t[T_CAP]), wa = hs_hist_spread(&t[T_WAIT]),
                      ds = hs_hist_spread(&t[T_DSP]), de = hs_hist_spread(&t[T_DETECTOR]);
    printf("SDRBENCH method=%s rotations=%u yields=%u rot_us=%" PRIu32 "/%" PRIu32 "/%" PRIu32
           " per_channel_hz_x10=%" PRIu32 " tune_us=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " cap_us=%" PRIu32 "/%" PRIu32
           "/%" PRIu32 " wait_us=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " dsp_us=%" PRIu32 "/%" PRIu32 "/%" PRIu32
           " detector_us=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " faults=%" PRIu32 " stale=%u\n",
           method == SDR_TUNE_FULL ? "full" : "pll", done, yields, rot.min, rot.med, rot.max, hz_x10(rot.med), tu.min,
           tu.med, tu.max, ca.min, ca.med, ca.max, wa.min, wa.med, wa.max, ds.min, ds.med, ds.max, de.min, de.med,
           de.max, sdr_rx_fault_count() - faults0, stale);
    pace(y, lines);
}

static void bench_timing(cp_t *cp, cp_yield_t *y, s_bench_t *b)
{
    unsigned lines = 0;
    printf("SDRBENCH start rotations=%u+%u: PLL tune, then full channel setup (about 6 s)\n", S_PLL_ROTATIONS,
           S_FULL_ROTATIONS);
    timing_run(cp, y, b, SDR_TUNE_PLL, S_PLL_ROTATIONS, &lines);
    timing_run(cp, y, b, SDR_TUNE_FULL, S_FULL_ROTATIONS, &lines);
    sdr_bench_set_tune_method(SDR_TUNE_PLL);
    printf("SDRBENCH done\n");
}

/* ---- K z: settle/residue after a hop off a strong LO ---- */

/* from_lo for 3 ms (one capture, discarded), then to_lo: capture Z_WORDS after d_us from the
 * tune's end and the level at off_hz in each quarter (IQ correction off). */
static bool residue_once(uint32_t from_lo, uint32_t to_lo, uint32_t d_us, int32_t off_hz, int32_t out[Z_QUARTERS])
{
    const uint32_t *w;
    if (sdr_bench_tune(from_lo, NULL) != ESP_OK) return false;
    if (sdr_bench_capture(CAP_WORDS, Z_STRONG_US, &w, NULL, NULL) != ESP_OK) return false;
    if (sdr_bench_tune(to_lo, NULL) != ESP_OK) return false;
    if (sdr_bench_capture(Z_WORDS, d_us, &w, NULL, NULL) != ESP_OK) return false;
    for (unsigned k = 0; k < Z_QUARTERS; k++) {
        sdr_psd_view_t v;
        uint8_t flags;
        if (sdr_bench_psd(w + k * (Z_WORDS / Z_QUARTERS), Z_WORDS / Z_QUARTERS, NFFT, -1, &v) != ESP_OK) return false;
        const int16_t l = sdr_bench_level(&v, off_hz, BAND_HALF_HZ, &flags);
        if (l == SDR_LEVEL_NONE) return false;
        out[k] = l;
    }
    return true;
}

static void residue_method(cp_yield_t *y, z_bench_t *b, sdr_tune_method_t method, const sdr_plan_t *p, unsigned s,
                           unsigned q, unsigned c, int32_t s_qdb, unsigned *lines)
{
    const char *name = method == SDR_TUNE_FULL ? "full" : "pll";
    unsigned jq = 0;
    const int hs = hop_of(p, s, NULL), hq = hop_of(p, q, &jq), hc = hop_of(p, c, NULL);
    const uint32_t lo_s = p->hop[hs].lo_khz, lo_q = p->hop[hq].lo_khz, lo_c = p->hop[hc].lo_khz;
    const int32_t off_q = p->hop[hq].offset_hz[jq];
    sdr_bench_set_tune_method(method);
    memset(b->sum, 0, sizeof(b->sum));
    memset(b->n, 0, sizeof(b->n));
    for (unsigned d = 0; d < Z_DELAYS; d++)
        for (unsigned rep = 0; rep < Z_REPEATS; rep++)
            for (unsigned k = 0; k < 2u; k++) {    /* after the strong LO, then the control, interleaved */
                int32_t lv[Z_QUARTERS];
                cp_yield(y);
                if (!residue_once(k ? lo_c : lo_s, lo_q, k_z_delay_us[d], off_q, lv)) continue;
                for (unsigned j = 0; j < Z_QUARTERS; j++) b->sum[k][d][j] += lv[j];
                b->n[k][d]++;
            }
    printf("SDRRESIDUE method=%s strong=R%u lo=%" PRIu32 " level_qdb=%" PRId32 " quiet=R%u lo=%" PRIu32
           " control=R%u lo=%" PRIu32 " gain=%u repeats=%u\n",
           name, s + 1u, lo_s / 1000u, s_qdb, q + 1u, lo_q / 1000u, c + 1u, lo_c / 1000u, (unsigned)sdr_rx_gain(),
           Z_REPEATS);
    pace(y, lines);
    for (unsigned d = 0; d < Z_DELAYS; d++) {
        int32_t v[2][Z_QUARTERS];
        char a[64], ctl[64];
        for (unsigned k = 0; k < 2u; k++)
            for (unsigned j = 0; j < Z_QUARTERS; j++) {
                const bool ok = b->n[k][d] > 0u;
                v[k][j] = ok ? div_round(b->sum[k][d][j], b->n[k][d]) : 0;
                /* A missing mean never counts as settled. */
                b->mean[k][d * Z_QUARTERS + j] = ok ? (int16_t)v[k][j] : (k ? INT16_MAX : INT16_MIN);
            }
        const bool have_a[Z_QUARTERS] = {b->n[0][d] > 0u, b->n[0][d] > 0u, b->n[0][d] > 0u, b->n[0][d] > 0u};
        const bool have_c[Z_QUARTERS] = {b->n[1][d] > 0u, b->n[1][d] > 0u, b->n[1][d] > 0u, b->n[1][d] > 0u};
        printf("SDRRESIDUE method=%s d_us=%u after_qdb=%s control_qdb=%s", name, (unsigned)k_z_delay_us[d],
               fmt_list(a, sizeof a, v[0], have_a, Z_QUARTERS), fmt_list(ctl, sizeof ctl, v[1], have_c, Z_QUARTERS));
        if (b->n[0][d] < Z_REPEATS || b->n[1][d] < Z_REPEATS)
            printf(" n=%u/%u", (unsigned)b->n[0][d], (unsigned)b->n[1][d]);
        printf("\n");
        pace(y, lines);
    }
    /* The first delay from which every later quarter of `after` stays within 1 dB of `control`. */
    const size_t from = hs_settled_from(b->mean[0], b->mean[1], Z_DELAYS * Z_QUARTERS, Z_TOL_QDB);
    const size_t d = (from + Z_QUARTERS - 1u) / Z_QUARTERS;
    if (d < Z_DELAYS) printf("SDRRESIDUE method=%s settle_us=%u\n", name, (unsigned)k_z_delay_us[d]);
    else printf("SDRRESIDUE method=%s settle_us=none (not within 1 dB at %u us)\n", name,
                (unsigned)k_z_delay_us[Z_DELAYS - 1u]);
    pace(y, lines);
}

static void bench_residue(cp_yield_t *y, z_bench_t *b)
{
    const sdr_plan_t *p = sdr_bench_plan();
    unsigned lines = 0;
    printf("SDRRESIDUE start: levels over %u rotations, then %u delays x %u repeats, PLL tune then full "
           "(about 5 s, VTX on and strong)\n", LV_ROTATIONS, (unsigned)Z_DELAYS, Z_REPEATS);
    int32_t med[SDR_RX_CHANNELS], cand[SDR_RX_CHANNELS];
    bool have[SDR_RX_CHANNELS];
    if (median_levels(y, &b->lv, LV_ROTATIONS, med, have, NULL) < LV_ROTATIONS) {
        printf("SDRRESIDUE stopped: the engine gave no rotation\nSDRRESIDUE done\n");
        return;
    }
    /* S: the strongest channel; Q: the quietest on another LO; C: the quietest on a third one. */
    for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) cand[ch] = have[ch] ? med[ch] : INT32_MIN;
    const unsigned s = (unsigned)hs_strongest_i32(cand, SDR_RX_CHANNELS);
    const int hs = hop_of(p, s, NULL);
    for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++)
        cand[ch] = have[ch] && hop_of(p, ch, NULL) != hs ? med[ch] : INT32_MAX;
    const unsigned q = (unsigned)hs_weakest_i32(cand, SDR_RX_CHANNELS);
    const bool have_q = cand[q] != INT32_MAX;
    const int hq = hop_of(p, q, NULL);
    for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) {
        const int h = hop_of(p, ch, NULL);
        cand[ch] = have[ch] && h != hs && h != hq ? med[ch] : INT32_MAX;
    }
    const unsigned c = (unsigned)hs_weakest_i32(cand, SDR_RX_CHANNELS);
    if (!have[s] || hs < 0 || !have_q || cand[c] == INT32_MAX) {
        printf("SDRRESIDUE refused: needs readings on 3 LOs\nSDRRESIDUE done\n");
        return;
    }
    if (med[s] - med[q] < 120)
        printf("SDRRESIDUE weak: R%u is only %.1f dB over R%u, bring the VTX closer (>= 30 dB)\n", s + 1u,
               (double)(med[s] - med[q]) / SDR_QDB_PER_DB, q + 1u);
    residue_method(y, b, SDR_TUNE_PLL, p, s, q, c, med[s], &lines);
    residue_method(y, b, SDR_TUNE_FULL, p, s, q, c, med[s], &lines);
    sdr_bench_set_tune_method(SDR_TUNE_PLL);
    printf("SDRRESIDUE done\n");
}

/* ---- K x: image rejection between the two channels of an LO ---- */

/* n captures at lo (tuned here), the PSD with plan hop iq_lo's IQ state (-1: none), the levels
 * at off_a (into va, unless NULL) and off_b (into vb); m (unless NULL): the plain moments of the
 * same captures. A capture with a sample at an ADC rail is left out and counted in *sat;
 * stop_on_sat: it ends the run. Returns the captures kept. */
static unsigned image_captures(cp_yield_t *y, uint32_t lo, int iq_lo, int32_t off_a, int32_t *va, int32_t off_b,
                               int32_t *vb, sdr_moments_t *m, bool stop_on_sat, unsigned *sat)
{
    unsigned kept = 0;
    *sat = 0;
    if (sdr_bench_tune(lo, NULL) != ESP_OK) return 0;
    for (unsigned attempt = 0; kept < X_CAPTURES && attempt < 2u * X_CAPTURES; attempt++) {
        cp_yield(y);
        const uint32_t *w;
        sdr_psd_view_t v;
        if (capture_at(lo, CAP_WORDS, &w) != ESP_OK || sdr_bench_psd(w, CAP_WORDS, NFFT, iq_lo, &v) != ESP_OK) continue;
        if (v.saturated) {
            (*sat)++;
            if (stop_on_sat) return kept;
            continue;
        }
        uint8_t flags;
        const int16_t a = va ? sdr_bench_level(&v, off_a, BAND_HALF_HZ, &flags) : 0;
        const int16_t bl = sdr_bench_level(&v, off_b, BAND_HALF_HZ, &flags);
        if (a == SDR_LEVEL_NONE || bl == SDR_LEVEL_NONE) continue;
        if (m) (void)sdr_bench_moments_add(w, CAP_WORDS, m);
        if (va) va[kept] = a;
        vb[kept] = bl;
        kept++;
    }
    return kept;
}

/* irr_<on|off>_db: the rejection as level(S) - excess of the partner over its floor, or a lower
 * bound when the partner is within X_FLOOR_DB of its floor. */
static const char *irr_text(char *buf, size_t size, const char *name, float s_db, float with_db, float floor_db)
{
    if (with_db - floor_db < X_FLOOR_DB)
        snprintf(buf, size, "%s>=%.1f (floor-limited)", name, (double)(s_db - floor_db + X_FLOOR_EXCESS_DB));
    else
        snprintf(buf, size, "%s=%.1f", name, (double)(s_db - hs_excess_db(with_db, floor_db)));
    return buf;
}

static void bench_image(cp_t *cp, cp_yield_t *y, x_bench_t *b)
{
    const sdr_plan_t *p = sdr_bench_plan();
    const uint8_t gain = sdr_rx_gain();
    int32_t med[SDR_RX_CHANNELS], over[SDR_RX_CHANNELS];
    bool have[SDR_RX_CHANNELS];
    char buf[96];
    if (median_levels(y, &b->lv, LV_ROTATIONS, med, have, NULL) < LV_ROTATIONS) {
        printf("SDRIMAGE stopped: the engine gave no rotation\nSDRIMAGE done\n");
        return;
    }
    const bool stored = cp->image_floor_gain && cp->image_floor_gain == gain;
    bool signal = false;
    for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) {
        const bool ok = stored && have[ch] && cp->image_floor_qdb[ch] != SDR_LEVEL_NONE;
        over[ch] = ok ? med[ch] - cp->image_floor_qdb[ch] : INT32_MIN;
        if (ok && over[ch] >= X_SIGNAL_QDB) signal = true;
    }
    if (!signal) {                                 /* run 1: VTX off, the floor of every channel */
        int32_t tmp[SDR_RX_CHANNELS], excess = 0;
        const int top = hs_over_median_i32(med, SDR_RX_CHANNELS, SDR_LEVEL_NONE, tmp, &excess);
        if (top >= 0 && excess > X_FLOOR_SPREAD_QDB) { /* a VTX is on: keep what was stored */
            printf("SDRIMAGE refused: R%d is %.1f dB over the channels' median (a floor allows %d dB), a VTX is on, "
                   "nothing stored: switch every VTX off, then K x\nSDRIMAGE done\n", top + 1,
                   (double)excess / SDR_QDB_PER_DB, X_FLOOR_SPREAD_QDB / SDR_QDB_PER_DB);
            return;
        }
        for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) cp->image_floor_qdb[ch] = (int16_t)med[ch];
        cp->image_floor_gain = gain;
        printf("SDRIMAGE floor stored gain=%u floor_qdb=%s\n", (unsigned)gain,
               fmt_list(buf, sizeof buf, med, have, SDR_RX_CHANNELS));
        printf("SDRIMAGE next: VTX on one channel at -10..-5 dBFS, then K x\nSDRIMAGE done\n");
        return;
    }
    /* Run 2: S is the channel highest over its floor, P shares its LO. */
    const unsigned s = (unsigned)hs_strongest_i32(over, SDR_RX_CHANNELS);
    if (over[s] < X_NEED_QDB) {
        printf("SDRIMAGE weak: bring the VTX closer (need >= 50 dB over the floor) strong=R%u over_db=%.1f\n"
               "SDRIMAGE done\n", s + 1u, (double)over[s] / SDR_QDB_PER_DB);
        return;
    }
    unsigned js = 0;
    const int hs = hop_of(p, s, &js);
    if (hs < 0 || p->hop[hs].nch != 2u) {
        printf("SDRIMAGE refused: R%u shares its LO with no other channel\nSDRIMAGE done\n", s + 1u);
        return;
    }
    const sdr_plan_hop_t *hop = &p->hop[hs];
    const unsigned jp = js ^ 1u, pc = hop->ch[jp];
    const uint32_t lo = hop->lo_khz;
    const int32_t floor_p = cp->image_floor_qdb[pc];
    if (floor_p == SDR_LEVEL_NONE) {
        printf("SDRIMAGE refused: no floor stored for R%u (VTX off, then K x)\nSDRIMAGE done\n", pc + 1u);
        return;
    }
    unsigned sat = 0, n_on, n_off, n_single;
    n_on = image_captures(y, lo, hs, hop->offset_hz[js], b->s_on, hop->offset_hz[jp], b->p_on, NULL, true, &sat);
    sdr_moments_t m = {0};
    if (!sat) n_off = image_captures(y, lo, -1, 0, NULL, hop->offset_hz[jp], b->p_off, &m, true, &sat);
    else n_off = 0;
    if (sat) {
        printf("SDRIMAGE clipped: move the VTX away\nSDRIMAGE done\n");
        return;
    }
    /* Diagnostic: P on its own LO, the SINGLE placement (no partner image on it). */
    const sdr_plan_cfg_t pc_cfg = sdr_plan_cfg_default();
    const int64_t p_khz = (int64_t)lo + hop->offset_hz[jp] / 1000;
    const int64_t step = pc_cfg.lo_step_khz ? pc_cfg.lo_step_khz : 1000;
    const int64_t lo_single = (p_khz + pc_cfg.single_lo_offset_khz + step / 2) / step * step;
    unsigned sat_single = 0;
    n_single = image_captures(y, (uint32_t)lo_single, -1, 0, NULL, (int32_t)((p_khz - lo_single) * 1000), b->p_single,
                              NULL, false, &sat_single);
    if (!n_on || !n_off) {
        printf("SDRIMAGE stopped: no capture at LO %" PRIu32 "\nSDRIMAGE done\n", lo / 1000u);
        return;
    }
    const int32_t s_on = hs_spread_i32(b->s_on, n_on).median, p_on = hs_spread_i32(b->p_on, n_on).median,
                  p_off = hs_spread_i32(b->p_off, n_off).median;
    const int32_t p_single = n_single ? hs_spread_i32(b->p_single, n_single).median : 0;
    const float s_db = (float)s_on / SDR_QDB_PER_DB, f_db = (float)floor_p / SDR_QDB_PER_DB;
    char on[48], off[48], single[24];
    if (n_single) snprintf(single, sizeof single, "%" PRId32, p_single);
    else snprintf(single, sizeof single, "none");
    printf("SDRIMAGE strong=R%u partner=R%u lo=%" PRIu32 " gain=%u strong_qdb=%" PRId32 " partner_on_qdb=%" PRId32
           " partner_off_qdb=%" PRId32 " floor_qdb=%" PRId32 " partner_single_qdb=%s %s %s moments_irr_db=%.1f"
           " ema_irr_db=%.1f",
           s + 1u, pc + 1u, lo / 1000u, (unsigned)gain, s_on, p_on, p_off, floor_p, single,
           irr_text(on, sizeof on, "irr_on_db", s_db, (float)p_on / SDR_QDB_PER_DB, f_db),
           irr_text(off, sizeof off, "irr_off_db", s_db, (float)p_off / SDR_QDB_PER_DB, f_db),
           (double)sdr_bench_moments_irr_db(&m), (double)sdr_bench_irr_db(hs));
    if (sat_single) printf(" single_clipped=%u", sat_single);
    printf("\nSDRIMAGE done\n");
}

/* ---- K n: noise against the sample count, and the borrowed-calibration DC control ---- */

static void bench_noise(cp_yield_t *y, n_bench_t *b)
{
    const sdr_plan_t *p = sdr_bench_plan();
    const uint8_t gain = sdr_rx_gain();
    unsigned lines = 0;
    uint32_t dc_cal = 0;
    char a[96], c[96];
    printf("SDRNOISE start: %u captures per LO of %u/%u/%u words, then a borrowed calibration (about 12 s, VTX off)\n",
           N_CAPTURES, k_n_words[0], k_n_words[1], k_n_words[2]);
    for (unsigned z = 0; z < N_SIZES; z++) {
        const unsigned words = k_n_words[z];
        uint16_t n[SDR_RX_CHANNELS] = {0};
        unsigned nd = 0, errors = 0, sat = 0;
        for (unsigned h = 0; h < p->nhops; h++) {
            const sdr_plan_hop_t *hop = &p->hop[h];
            if (sdr_bench_tune(hop->lo_khz, NULL) != ESP_OK) { errors++; continue; }
            for (unsigned k = 0; k < N_CAPTURES; k++) {
                cp_yield(y);
                const uint32_t *w;
                sdr_psd_view_t v;
                if (capture_at(hop->lo_khz, words, &w) != ESP_OK) { errors++; continue; }
                const int64_t t0 = esp_timer_get_time();
                if (sdr_bench_psd(w, words, NFFT, (int)h, &v) != ESP_OK) { errors++; continue; }
                for (unsigned j = 0; j < hop->nch; j++) {
                    uint8_t flags;
                    const unsigned ch = hop->ch[j];
                    const int16_t l = sdr_bench_level(&v, hop->offset_hz[j], BAND_HALF_HZ, &flags);
                    if (ch < SDR_RX_CHANNELS && l != SDR_LEVEL_NONE && n[ch] < N_CAPTURES) b->lv[ch][n[ch]++] = l;
                }
                if (nd < SDR_RX_MAX_HOPS * N_CAPTURES) b->dsp_us[nd++] = (uint32_t)(esp_timer_get_time() - t0);
                if (v.saturated) sat++;
                if (words == CAP_WORDS) {          /* the production size: the calibrated LOs' DC */
                    const float di = fabsf(v.dc_i), dq = fabsf(v.dc_q);
                    const uint32_t dc = (uint32_t)lroundf(di > dq ? di : dq);
                    if (dc > dc_cal) dc_cal = dc;
                }
            }
        }
        int32_t mean[SDR_RX_CHANNELS], sd[SDR_RX_CHANNELS];
        bool have[SDR_RX_CHANNELS];
        size_t len = 0;
        for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) {
            uint32_t sd_mdb = 0;
            hs_mean_sd_qdb(b->lv[ch], n[ch], &mean[ch], &sd_mdb);
            sd[ch] = (int32_t)sd_mdb;
            have[ch] = n[ch] > 0u;
        }
        for (unsigned ch = 0; ch < SDR_RX_CHANNELS && len < sizeof c; ch++) {
            const int w = have[ch] ? snprintf(c + len, sizeof c - len, "%s%.2f", ch ? "/" : "", (double)sd[ch] / 1000.0)
                                   : snprintf(c + len, sizeof c - len, "%snone", ch ? "/" : "");
            if (w < 0) break;
            len += (size_t)w;
        }
        printf("SDRNOISE n=%u segments=%u dsp_us=%" PRIu32 " sd_db=%s mean_qdb=%s", words, words / NFFT,
               hs_spread(b->dsp_us, nd).med, c, fmt_list(a, sizeof a, mean, have, SDR_RX_CHANNELS));
        if (errors || sat) printf(" errors=%u clipped=%u", errors, sat);
        printf("\n");
        pace(y, &lines);
    }
    /* ONCE emulation: the last LO with the first LO's DC/IQ tables (a band edge with the other
     * edge's calibration), as widscan58's RECALMODE ONCE: the DC that CAL_NEEDED must catch. */
    const sdr_plan_hop_t *edge = &p->hop[p->nhops - 1u];
    const uint32_t lo = edge->lo_khz, from = p->hop[0].lo_khz;
    printf("SDRNOISE dc gain=%u calibrated_dc_max=%" PRIu32 " threshold=%u borrowed lo=%" PRIu32 " cal_from=%" PRIu32,
           (unsigned)gain, dc_cal, (unsigned)CONFIG_SDR_RX_DC_CAL_NEEDED, lo / 1000u, from / 1000u);
    const esp_err_t err = p->nhops > 1u ? sdr_bench_borrow_cal(lo, from) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        printf(" refused=%s\n", esp_err_to_name(err));
    } else {
        float dc_i = 0.0f, dc_q = 0.0f;
        uint16_t n[SDR_RX_PER_HOP] = {0};
        unsigned stale = 0;
        for (unsigned k = 0; k < N_BORROW_CAPTURES; k++) {
            cp_yield(y);
            const uint32_t *w;
            sdr_psd_view_t v;
            const esp_err_t ce = sdr_bench_capture(CAP_WORDS, CONFIG_SDR_RX_SETTLE_US, &w, NULL, NULL);
            if (ce == ESP_ERR_INVALID_RESPONSE) { stale = 1; break; }   /* PHY tracking replaced the tables */
            if (ce != ESP_OK || sdr_bench_psd(w, CAP_WORDS, NFFT, -1, &v) != ESP_OK) continue;
            if (fabsf(v.dc_i) > fabsf(dc_i)) dc_i = v.dc_i;
            if (fabsf(v.dc_q) > fabsf(dc_q)) dc_q = v.dc_q;
            for (unsigned j = 0; j < edge->nch && j < SDR_RX_PER_HOP; j++) {
                uint8_t flags;
                const int16_t l = sdr_bench_level(&v, edge->offset_hz[j], BAND_HALF_HZ, &flags);
                if (l != SDR_LEVEL_NONE) b->lv[j][n[j]++] = l;
            }
        }
        int32_t mean[SDR_RX_PER_HOP] = {0};
        bool have[SDR_RX_PER_HOP] = {false};
        for (unsigned j = 0; j < edge->nch && j < SDR_RX_PER_HOP; j++) {
            uint32_t sd_mdb;
            hs_mean_sd_qdb(b->lv[j], n[j], &mean[j], &sd_mdb);
            have[j] = n[j] > 0u;
        }
        printf(" dc_i=%ld dc_q=%ld mean_qdb=%s%s\n", lroundf(dc_i), lroundf(dc_q),
               fmt_list(a, sizeof a, mean, have, edge->nch < SDR_RX_PER_HOP ? edge->nch : SDR_RX_PER_HOP),
               stale ? " stale=1" : "");
    }
    sdr_bench_borrow_end();
    printf("SDRNOISE done\n");
}

/* ---- K p: the SINGLE plan against PAIRS ---- */

static void print_plan(cp_yield_t *y, const char *mode, unsigned *lines)
{
    const sdr_plan_t *p = sdr_bench_plan();
    printf("SDRPLAN mode=%s hops=%u\n", mode, p->nhops);
    pace(y, lines);
    for (unsigned h = 0; h < p->nhops; h++) {
        const sdr_plan_hop_t *hop = &p->hop[h];
        printf("SDRPLAN hop=%u lo=%" PRIu32, h, hop->lo_khz / 1000u);
        for (unsigned j = 0; j < hop->nch; j++)
            printf(" R%u=%+.1fMHz/bins%d..%d", hop->ch[j] + 1u, (double)hop->offset_hz[j] / 1e6, hop->band[j].r0,
                   hop->band[j].r1);
        printf("\n");
        pace(y, lines);
    }
}

static void bench_plan(cp_yield_t *y, lv_buf_t *b)
{
    unsigned lines = 0;
    int32_t pairs[SDR_RX_CHANNELS], single[SDR_RX_CHANNELS];
    bool have_p[SDR_RX_CHANNELS], have_s[SDR_RX_CHANNELS];
    uint32_t rot_p = 0, rot_s = 0;
    char a[96], c[96];
    printf("SDRPLAN start: %u rotations in PAIRS, then %u in SINGLE (its new LOs calibrate first), not saved\n",
           LV_ROTATIONS, P_SINGLE_ROTATIONS);
    print_plan(y, "pairs", &lines);
    const unsigned np = median_levels(y, b, LV_ROTATIONS, pairs, have_p, &rot_p);
    sdr_bench_set_plan(SDR_PLAN_SINGLE);
    print_plan(y, "single", &lines);
    const unsigned ns = median_levels(y, b, P_SINGLE_ROTATIONS, single, have_s, &rot_s);
    printf("SDRPLAN compare gain=%u pairs_qdb=%s single_qdb=%s pairs_hz_x10=%" PRIu32 " single_hz_x10=%" PRIu32
           " rotations=%u/%u\n", (unsigned)sdr_rx_gain(), fmt_list(a, sizeof a, pairs, have_p, SDR_RX_CHANNELS),
           fmt_list(c, sizeof c, single, have_s, SDR_RX_CHANNELS), hz_x10(rot_p), hz_x10(rot_s), np, ns);
    printf("SDRPLAN done (PAIRS again)\n");
}

/* ---- K S: scan recording ---- */

/* order=: the hops in reading order, a+b for channels sharing one capture. */
static const char *plan_order(char *buf, size_t size)
{
    const sdr_plan_t *p = sdr_bench_plan();
    size_t len = 0;
    buf[0] = '\0';
    for (unsigned h = 0; h < p->nhops && len < size; h++)
        for (unsigned j = 0; j < p->hop[h].nch && len < size; j++) {
            const int w = snprintf(buf + len, size - len, "%s%u", j ? "+" : h ? ":" : "", p->hop[h].ch[j] + 1u);
            if (w < 0) return buf;
            len += (size_t)w;
        }
    return buf;
}

static float kept_s(const scan_row_t *ring, size_t cap, uint32_t rows, unsigned decim, uint32_t rot_us)
{
    const size_t kept = hs_ring_kept(rows, cap), oldest = hs_ring_oldest(rows, cap);
    if (!kept) return 0.0f;
    const uint32_t newest = ring[(oldest + kept - 1u) % cap].t_us;
    return (float)(newest - ring[oldest].t_us + decim * rot_us) / 1e6f;
}

static void bench_record(cp_t *cp, cp_yield_t *y, scan_row_t *ring, size_t cap)
{
    sdr_rx_health_t h;
    sdr_rx_get_health(&h);
    const uint32_t rate = h.rot_hz_x10 ? h.rot_hz_x10 : R_NOMINAL_HZ_X10;
    /* decim = ceil(rotations per second x R_KEEP_S / rows): the ring covers >= R_KEEP_S. */
    unsigned decim = (unsigned)(((uint64_t)rate * R_KEEP_S + 10u * cap - 1u) / (10u * cap));
    if (!decim) decim = 1;
    const uint8_t gain = sdr_rx_gain();
    char order[4u * SDR_RX_CHANNELS];
    plan_order(order, sizeof order);
    printf("SCANBENCH start decim=%u row_ms=%.1f rows=%u keep_s=%.0f max_s=%u order=%s gain=%u: "
           "K then S stops; no detection meanwhile\n", decim, (double)decim * 1e4 / rate, (unsigned)cap,
           (double)cap * decim * 10.0 / rate, R_MAX_S, order, (unsigned)gain);
    rotation_t r;
    uint32_t rotations = 0, rows = 0, rot_med = h.rot_us_avg, live_rot = 0;   /* the last 1 s window's */
    const uint32_t faults0 = sdr_rx_fault_count();
    bool running = next_rotation(y, &r, NULL, NULL);          /* whole rotations from here */
    const char *stop = running ? "key" : "engine";
    const int64_t t0 = esp_timer_get_time();
    int64_t rot_start = t0, live_t = t0, next_live = t0 + R_LIVE_S * 1000000ll;
    int32_t sum[SDR_RX_CHANNELS] = {0};
    uint16_t cnt[SDR_RX_CHANNELS] = {0};
    unsigned sat[R_SAT_HOPS] = {0};
    int16_t peak[SDR_RX_CHANNELS];
    unsigned in_row = 0;
    uint32_t row_t = 0;
    for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) peak[ch] = SDR_LEVEL_NONE;
    while (running && !cp->scan_stop) {           /* scan_stop: a second K S */
        if (esp_timer_get_time() - t0 >= R_MAX_S * 1000000ll) { stop = "max_s"; break; }
        if (!next_rotation(y, &r, NULL, NULL)) { stop = "engine"; break; }
        const int64_t end = esp_timer_get_time();
        for (unsigned i = 0; i < r.n; i++) {
            const unsigned ch = r.r[i].channel;
            const int16_t l = r.r[i].level_qdb;
            if (ch >= SDR_RX_CHANNELS || l == SDR_LEVEL_NONE) continue;
            sum[ch] += l;
            cnt[ch]++;
            if (l > peak[ch]) peak[ch] = l;
        }
        for (unsigned j = 0; j < R_SAT_HOPS; j++) sat[j] += (r.sat_hops >> j) & 1u;
        /* A running median of the rotation time (1 us a step from the engine's last average): the
         * in-row channel times of tools/scan_analyze.py need it to ~0.1 ms, and the ring fills the
         * scratch, so the rotation times are not kept. */
        const uint32_t rot_us = (uint32_t)(end - rot_start);
        rot_med = !rot_med ? rot_us : rot_us > rot_med ? rot_med + 1u : rot_us < rot_med ? rot_med - 1u : rot_med;
        if (!in_row) row_t = (uint32_t)(rot_start - t0);
        rot_start = end;
        rotations++;
        if (++in_row == decim) {
            scan_row_t *e = &ring[rows % cap];
            e->t_us = row_t;
            for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) {
                e->qdb[ch] = cnt[ch] ? (int16_t)div_round(sum[ch], cnt[ch]) : SDR_LEVEL_NONE;
                sum[ch] = 0;
                cnt[ch] = 0;
            }
            uint16_t packed = 0;
            for (unsigned j = 0; j < R_SAT_HOPS; j++) {
                packed |= (uint16_t)((sat[j] < R_SAT_MAX ? sat[j] : R_SAT_MAX) << (4u * (R_SAT_HOPS - 1u - j)));
                sat[j] = 0;
            }
            e->sat = packed;
            rows++;
            in_row = 0;
        }
        if (end >= next_live) {
            int32_t pk[SDR_RX_CHANNELS];
            bool have[SDR_RX_CHANNELS];
            char a[96];
            for (unsigned ch = 0; ch < SDR_RX_CHANNELS; ch++) {
                pk[ch] = peak[ch];
                have[ch] = peak[ch] != SDR_LEVEL_NONE;
                peak[ch] = SDR_LEVEL_NONE;
            }
            printf("SCANBENCH live t_s=%lld rotations=%" PRIu32 " per_channel_hz_x10=%" PRIu32 " faults=%" PRIu32
                   " peak_qdb=%s kept_s=%.1f\n", (long long)((end - t0) / 1000000), rotations,
                   (uint32_t)(((uint64_t)(rotations - live_rot) * 10000000u) / (uint64_t)(end - live_t)),
                   sdr_rx_fault_count() - faults0, fmt_list(a, sizeof a, pk, have, SDR_RX_CHANNELS),
                   (double)kept_s(ring, cap, rows, decim, rot_med));
            live_rot = rotations;
            live_t = end;
            next_live += R_LIVE_S * 1000000ll;
        }
    }
    /* The dump, from this task: no capture meanwhile. stdout never blocks (main.c), so what
     * overflows the USB FIFO is dropped: paced, after a pause for what other tasks print. */
    vTaskDelay(pdMS_TO_TICKS(R_DUMP_PAUSE_MS));
    const size_t kept = hs_ring_kept(rows, cap), oldest = hs_ring_oldest(rows, cap);
    unsigned lines = 0;
    printf("SCANDUMP begin rotations=%" PRIu32 " kept=%u kept_s=%.1f decim=%u format=qdb order=%s gain=%u rot_us=%" PRIu32
           " fw=%s\n", rotations, (unsigned)kept, (double)kept_s(ring, cap, rows, decim, rot_med), decim, order,
           (unsigned)gain, rot_med, firmware_id());
    for (size_t i = 0; i < kept; i++) {
        const scan_row_t *e = &ring[(oldest + i) % cap];
        printf("SCAN,%" PRIu32 ",%d,%d,%d,%d,%d,%d,%d,%d,%04x\n", e->t_us, e->qdb[0], e->qdb[1], e->qdb[2], e->qdb[3],
               e->qdb[4], e->qdb[5], e->qdb[6], e->qdb[7], (unsigned)e->sat);
        pace(y, &lines);
    }
    printf("SCANDUMP end rotations=%" PRIu32 " decim=%u format=qdb order=%s gain=%u\n", rotations, decim, order,
           (unsigned)gain);   /* the begin line can get lost */
    printf("SCANBENCH done stop=%s rotations=%" PRIu32 " rows=%" PRIu32 " faults=%" PRIu32 "\n", stop, rotations, rows,
           sdr_rx_fault_count() - faults0);
}

void cp_bench_run(cp_t *cp, char key, cp_yield_t *y)
{
    size_t bytes = 0;
    void *scratch = sdr_bench_scratch(&bytes);
    const sdr_plan_t *p = sdr_bench_plan();
    if (!scratch || bytes < SCRATCH_BYTES || !p || !p->nhops) {
        printf("CHECKPOINT bench=%c refused=no_engine\n", key);
        return;
    }
    switch (key) {
    case 's': bench_timing(cp, y, scratch); break;
    case 'z': bench_residue(y, scratch); break;
    case 'x': bench_image(cp, y, scratch); break;
    case 'n': bench_noise(y, scratch); break;
    case 'p': bench_plan(y, scratch); break;
    case 'S': bench_record(cp, y, scratch, bytes / sizeof(scan_row_t)); break;
    default: printf("CHECKPOINT bench=%c refused=unknown\n", key); break;
    }
}
