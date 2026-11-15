/* Host test for components/pass_detector's scan detector, fed per-channel readings at the
 * sdr_rx rate (490 Hz per channel, R1..R8 in LO pairs) and at the old 57 Hz sweep, then the pair
 * guard (sd_feed_hop) on a reading model of a clipping pair (test_pair_ghost.c runs it on the
 * real DSP).
 * Build/run:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
 *      test_scan_detector.c ../components/pass_detector/scan_detector.c \
 *      ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
 */
#include "scan_detector.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { printf("ok   %s\n", name); g_pass++; }                    \
        else { printf("FAIL %s\n", name); g_fail++; }                         \
    } while (0)

#define RATE_A_US 2040     /* 490 Hz per channel: sdr_rx, 4 hops of two channels */
#define RATE_B_US 17500    /* 57 Hz: the sweep the defaults were tuned at */
#define QUIET_DB  (-40.0f) /* a quiet channel, dBFS */
#define NOISE_DB  1.2f     /* sd of one reading */
#define T0_US     1000000  /* the first reading: boot is not at 0 */
#define S(x)      ((int64_t)((x) * 1e6))

/* Deterministic noise: 64-bit LCG, Box-Muller. */
static uint64_t s_lcg;
static void seed(uint64_t s) { s_lcg = s * 0x9E3779B97F4A7C15ull + 1u; }
static double uniform(void)
{
    s_lcg = s_lcg * 6364136223846793005ull + 1442695040888963407ull;
    return ((double)(s_lcg >> 11) + 0.5) / 9007199254740992.0;
}
static float gauss(float sd)
{
    const double u = uniform(), v = uniform();
    return (float)(sd * sqrt(-2.0 * log(u)) * cos(6.283185307179586 * v));
}

/* What a channel shows above QUIET_DB: rectangles (sigma 0) and Gaussian passes. */
typedef struct {
    unsigned ch;
    float db;
    int64_t from, to;   /* rectangle */
    int64_t centre;     /* Gaussian, sigma_us > 0 */
    float sigma_us;
} bump_t;
typedef struct {
    unsigned n;
    bump_t b[12];
} scene_t;

static float scene_db(const scene_t *s, unsigned ch, int64_t t)
{
    float v = 0.0f;
    for (unsigned k = 0; s && k < s->n; k++) {
        const bump_t *b = &s->b[k];
        if (b->ch != ch) continue;
        if (b->sigma_us > 0.0f) {
            const float x = (float)(t - b->centre) / b->sigma_us;
            v += b->db * expf(-0.5f * x * x);
        } else if (t >= b->from && t < b->to) {
            v += b->db;
        }
    }
    return v;
}

static bump_t rect(unsigned ch, float db, double from_s, double to_s)
{
    return (bump_t){.ch = ch, .db = db, .from = T0_US + S(from_s), .to = T0_US + S(to_s)};
}
static bump_t gpass(unsigned ch, float db, double centre_s, float sigma_ms)
{
    return (bump_t){.ch = ch, .db = db, .centre = T0_US + S(centre_s), .sigma_us = sigma_ms * 1000.0f};
}

typedef struct {
    scan_detector_t sd;
    int64_t t;            /* start of the next rotation */
    int32_t dt;           /* rotation period = reading interval of a channel */
    float noise;
    uint8_t sat_mask;     /* channels whose readings carry SD_FLAG_SATURATED */
    uint8_t only_mask;    /* channels fed (0: all) */
    unsigned passes[PD_CHANNELS], total;
    pd_event_t last;
    uint32_t reads[PD_CHANNELS];
    double rise_sum;      /* idle statistics, past learning */
    uint64_t rise_n;
    float rise_max[PD_CHANNELS];
} run_t;

static float s_rearm_db;   /* the relative re-arm start() and pair_run() set (0: off, as sd_init leaves it) */

static void start(run_t *r, int32_t dt_us, float noise, uint64_t s)
{
    memset(r, 0, sizeof(*r));
    r->t = T0_US;
    r->dt = dt_us;
    r->noise = noise;
    for (unsigned ch = 0; ch < PD_CHANNELS; ch++) r->rise_max[ch] = -1e9f;
    seed(s);
    const pd_config_t c = sd_config_default();
    sd_init(&r->sd, &c);
    pd_set_rearm(&r->sd.det, s_rearm_db);
}

static int16_t qdb(float db) { return (int16_t)lroundf(4.0f * db); }

static void feed(run_t *r, unsigned ch, int16_t q, int64_t t)
{
    pd_event_t e;
    const uint8_t flags = (r->sat_mask >> ch) & 1u ? SD_FLAG_SATURATED : 0u;
    if (sd_feed(&r->sd, ch, q, flags, t, &e)) {
        r->passes[e.channel]++;
        r->total++;
        r->last = e;
    }
    r->reads[ch]++;
}

/* One rotation as sdr_rx gives it: R1 R2 at the first hop's capture, R3 R4 a quarter
 * rotation later, and so on; then sd_rotation. */
static void rotation(run_t *r, const scene_t *s)
{
    for (unsigned ch = 0; ch < PD_CHANNELS; ch++) {
        if (r->only_mask && !((r->only_mask >> ch) & 1u)) continue;
        const int64_t t = r->t + (int64_t)(ch / 2u) * r->dt / 4;
        feed(r, ch, qdb(QUIET_DB + scene_db(s, ch, t) + gauss(r->noise)), t);
    }
    const int64_t end = r->t + 3 * (int64_t)r->dt / 4;
    sd_rotation(&r->sd, end);
    if (!sd_learning(&r->sd, end)) {
        for (unsigned ch = 0; ch < PD_CHANNELS; ch++) {
            const float rise = sd_rise_db(&r->sd, ch);
            if (rise > r->rise_max[ch]) r->rise_max[ch] = rise;
            if (!r->only_mask || ((r->only_mask >> ch) & 1u)) { r->rise_sum += rise; r->rise_n++; }
        }
    }
    r->t += r->dt;
}

/* Rotations until T0 + until_s. */
static void play(run_t *r, double until_s, const scene_t *s)
{
    while (r->t < T0_US + S(until_s)) rotation(r, s);
}

static void reset_max(run_t *r)
{
    for (unsigned ch = 0; ch < PD_CHANNELS; ch++) r->rise_max[ch] = -1e9f;
}

static bool same_channel(const sd_channel_t *a, const sd_channel_t *b)
{
    return a->nhist == b->nhist && a->hidx == b->hidx && !memcmp(a->hist, b->hist, sizeof(a->hist)) &&
           a->primed == b->primed && a->smooth_db == b->smooth_db && a->last_us == b->last_us &&
           a->dt_avg_us == b->dt_avg_us && a->rest_db == b->rest_db && a->rest_primed == b->rest_primed;
}

/* One +15 dB pass (sigma 150 ms) on R4 at 8 s, from a quiet start; the result of seed s. */
typedef struct {
    unsigned total, on_r4;
    float peak_db, lag_ms, err_ms;
} pass_result_t;

static pass_result_t one_pass(int32_t dt_us, uint64_t s)
{
    static run_t r;
    start(&r, dt_us, NOISE_DB, s);
    const scene_t sc = {1, {gpass(3, 15.0f, 8.0, 150.0f)}};
    play(&r, 12.0, &sc);
    const double centre_ms = (T0_US + S(8.0)) / 1000.0;
    pass_result_t o = {.total = r.total, .on_r4 = r.passes[3], .peak_db = r.last.peak_db};
    o.lag_ms = (float)(r.last.peak_ms - centre_ms);
    o.err_ms = (float)(r.last.peak_ms - sd_delay_ms(&r.sd, 3) - centre_ms);
    return o;
}

/* ---- Pair guard (sd_feed_hop): a reading model of one PAIRS hop (R5 at -19 MHz, R6 at +18 MHz).
 * Band levels in dBFS: the floor -59.5 dB, 1.2 dB per reading; a drone is 40 dB up at 0.9 m for
 * 25 mW, -20 log10(d), +13.8 dB at 600 mW, 20 m/s (or v_ms), 1 dB of fading per capture. A capture clips once
 * R6 stands over PAIR_CLIP_DBFS (the first rail hit of a 12 MHz-deviation VTX); then R6 reads
 * compressed and R5 reads R6's clipping products, R6's reading + X, X drawn from PAIR_CLIP_X (the
 * quantiles of R5 - R6 on captures clipped 1-10 dB deep in a host simulation of the C5 capture
 * through sdr_psd.c, as test_pair_ghost.c builds them), plus a real R5 signal 3 dB down (the rails
 * suppress the weaker). The other six channels read the floor. */
#define PAIR_LO        4u        /* R5 */
#define PAIR_HI        5u        /* R6 */
#define PAIR_HOP       2u        /* LO 5825 */
#define PAIR_FLOOR     (-59.5)
#define PAIR_CLIP_DBFS (-5.3)
static const double PAIR_CLIP_X[][2] = {{0.0, -45}, {0.5, -28}, {0.75, -17}, {0.88, -13}, {0.95, -3}, {0.99, 10}, {1.0, 12}};

typedef struct {
    double mw, d_m, t0_s;    /* mw 0: absent */
    int laps;
    double lap_s;
    double hover_d_m, hover_t0_s, hover_t1_s;   /* hover_d_m > 0: parked at that distance meanwhile */
    double v_ms;                                /* speed past the gate, 0 = 20 m/s */
} pdrone_t;
typedef struct {
    unsigned n[PD_CHANNELS];
    double peak_s[PD_CHANNELS][8];   /* scene time of each pass's peak_ms */
    float peak_db[PD_CHANNELS][8];
    unsigned held[PD_CHANNELS];
} pair_out_t;

static double pdrone_db(const pdrone_t *d, double ts)
{
    if (d->mw <= 0.0) return -999.0;
    const double base = -19.5 + 10.0 * log10(d->mw / 25.0);
    if (d->hover_d_m > 0.0) {   /* flies in at 20 m/s, parks, flies off */
        const double away = ts < d->hover_t0_s ? d->hover_t0_s - ts : ts > d->hover_t1_s ? ts - d->hover_t1_s : 0.0;
        return base - 20.0 * log10((d->hover_d_m + 20.0 * away) / 0.9) - (away > 0.75 ? 40.0 * (away - 0.75) : 0.0);
    }
    double best = -999.0;
    for (int k = 0; k < (d->laps ? d->laps : 1); k++) {
        const double dt = ts - (d->t0_s + k * d->lap_s), x = (d->v_ms > 0.0 ? d->v_ms : 20.0) * dt;
        double v = base - 10.0 * log10((d->d_m * d->d_m + x * x) / 0.81);
        if (fabs(dt) > 0.75) v -= 40.0 * (fabs(dt) - 0.75);   /* off round the track */
        if (v > best) best = v;
    }
    return best;
}
static double psum(double a, double b) { return 10.0 * log10(pow(10.0, a / 10.0) + pow(10.0, b / 10.0)); }
static double clip_x(void)
{
    const double u = uniform();
    unsigned k = 1;
    while (PAIR_CLIP_X[k][0] < u) k++;
    const double *a = PAIR_CLIP_X[k - 1], *b = PAIR_CLIP_X[k];
    return a[1] + (b[1] - a[1]) * (u - a[0]) / (b[0] - a[0]);
}

/* 3.5 s of floor (the rests settle), then dur_s of the scene; guard: sd_feed_hop, else sd_feed per
 * reading (cp_scan.c before the guard). Scene time 0 = T0 + 3.5 s. */
static void pair_run(const pdrone_t *hi, const pdrone_t *lo, int guard, uint64_t s, double dur_s, pair_out_t *o,
                     scan_detector_t *sd)
{
    memset(o, 0, sizeof(*o));
    const pd_config_t c = sd_config_default();
    sd_init(sd, &c);
    pd_set_rearm(&sd->det, s_rearm_db);
    seed(s);
    for (int64_t t = T0_US; t < T0_US + S(3.5 + dur_s); t += RATE_A_US) {
        for (unsigned hop = 0; hop < 4u; hop++) {
            const int64_t th = t + (int64_t)hop * RATE_A_US / 4;
            const double ts = (double)(th - T0_US) * 1e-6 - 3.5;
            double ra = PAIR_FLOOR + gauss(1.2f), rb = PAIR_FLOOR + gauss(1.2f);
            uint8_t fl = 0;
            if (hop == PAIR_HOP && ts >= 0.0) {
                const double b = pdrone_db(hi, ts) + gauss(1.0f), a = pdrone_db(lo, ts) + gauss(1.0f);
                if (b > PAIR_CLIP_DBFS) {
                    fl = SD_FLAG_SATURATED;
                    rb = PAIR_CLIP_DBFS + 7.5 * tanh((b - PAIR_CLIP_DBFS) / 10.0) + gauss(0.3f);
                    ra = psum(psum(ra, a - 3.0), rb + clip_x());
                } else {
                    ra = psum(ra, a);
                    rb = psum(rb, b);
                }
            }
            const sd_reading_t r[2] = {{th, qdb((float)ra), (uint8_t)(2u * hop), fl},
                                       {th, qdb((float)rb), (uint8_t)(2u * hop + 1u), fl}};
            pd_event_t ev[2];
            unsigned rang = 0, held = 0;
            if (guard) {
                rang = sd_feed_hop(sd, hop, r, 2, ev, &held);
            } else {
                for (unsigned i = 0; i < 2; i++)
                    if (sd_feed(sd, r[i].channel, r[i].level_qdb, r[i].flags, r[i].t_us, &ev[i])) rang |= 1u << i;
            }
            for (unsigned i = 0; i < 2; i++) {
                const unsigned ch = r[i].channel;
                if (held & (1u << i)) o->held[ch]++;
                if ((rang & (1u << i)) && o->n[ch] < 8u) {
                    o->peak_s[ch][o->n[ch]] = ev[i].peak_ms / 1000.0 - (T0_US / 1e6 + 3.5);
                    o->peak_db[ch][o->n[ch]] = ev[i].peak_db;
                    o->n[ch]++;
                }
            }
        }
        sd_rotation(sd, t + 3 * RATE_A_US / 4);
    }
}

/* A pilot that laps a pair channel while the other one clips (the reading model above): the pilot
 * stands `base` dB over the floor from 4 s (none: 0) and passes +20 dB over it (sigma 150 ms) at 5,
 * 11, 17 and 23 s; from STEP_CLIP_S, `after` dB instead. The other drone clips the capture from
 * STEP_CLIP_S for clip_s, switched on and off at once (a VTX armed by the antenna) or flown in and
 * out over 0.5 s (ramp). The pilot is R5 and the clipping drone R6, or the other way round (mirror).
 * Every run has the relative re-arm at `rearm`. */
#define STEP_CLIP_S 13.5
typedef struct {
    double base, after, clip_s;
    int ramp, mirror;
    float rearm;
} step_t;
typedef struct {
    unsigned n, n_src, held, held_src;   /* the pilot's passes and held readings; the clipping drone's */
    double t[8];                         /* the pilot's pass times (peak_ms), s */
    pd_channel_t probe;                  /* the pilot's pd state 0.1 s after the clip */
    float probe_rest_db;
} step_out_t;

static void step_run(const step_t *p, uint64_t s, step_out_t *o)
{
    static scan_detector_t sd;
    memset(o, 0, sizeof(*o));
    const pd_config_t c = sd_config_default();
    sd_init(&sd, &c);
    pd_set_rearm(&sd.det, p->rearm);
    seed(s);
    const unsigned pilot = p->mirror ? PAIR_HI : PAIR_LO, src = p->mirror ? PAIR_LO : PAIR_HI;
    const double end = STEP_CLIP_S + p->clip_s;
    bool probed = false;
    for (int64_t t = T0_US; t < T0_US + S(26.0); t += RATE_A_US) {
        for (unsigned hop = 0; hop < 4u; hop++) {
            const int64_t th = t + (int64_t)hop * RATE_A_US / 4;
            const double ts = (double)(th - T0_US) * 1e-6;
            double rp = PAIR_FLOOR + gauss(1.2f), rs = PAIR_FLOOR + gauss(1.2f);
            uint8_t fl = 0;
            if (hop == PAIR_HOP && ts >= 4.0) {
                const double base = ts < STEP_CLIP_S ? p->base : p->after;
                double v = base > 0.0 ? base : -999.0;
                for (int k = 0; k < 4; k++) {
                    const double x = (ts - (5.0 + 6.0 * k)) / 0.15;
                    v = fmax(v, (base > 0.0 ? base : 0.0) + 20.0 * exp(-0.5 * x * x));
                }
                const double a = PAIR_FLOOR + v + gauss(1.0f);
                if (ts >= STEP_CLIP_S && ts < end) {
                    fl = SD_FLAG_SATURATED;
                    rs = PAIR_CLIP_DBFS + 5.0 + gauss(0.3f);
                    rp = psum(psum(rp, a - 3.0), rs + clip_x());
                } else {
                    rp = psum(rp, a);
                    const double away = ts < STEP_CLIP_S ? STEP_CLIP_S - ts : ts - end;
                    if (p->ramp && away < 0.5) rs = psum(rs, PAIR_CLIP_DBFS - 1.0 - 60.0 * away + gauss(1.0f));
                }
            }
            if (hop == PAIR_HOP && ts >= end + 0.1 && !probed) {
                probed = true;
                o->probe = sd.det.ch[pilot];
                o->probe_rest_db = sd.c[pilot].rest_db;
            }
            sd_reading_t rd[2];   /* slot 0 the hop's lower channel (R5 on PAIR_HOP), 1 its upper */
            rd[pilot & 1u] = (sd_reading_t){th, qdb((float)rp), (uint8_t)(2u * hop + (pilot & 1u)), fl};
            rd[src & 1u] = (sd_reading_t){th, qdb((float)rs), (uint8_t)(2u * hop + (src & 1u)), fl};
            pd_event_t ev[2];
            const unsigned got = sd_feed_hop(&sd, hop, rd, 2, ev, NULL);
            for (unsigned i = 0; i < 2; i++) {
                if (!(got & (1u << i))) continue;
                if (ev[i].channel == pilot && o->n < 8u) o->t[o->n++] = ev[i].peak_ms / 1000.0 - T0_US / 1e6;
                o->n_src += ev[i].channel == src;
            }
        }
        sd_rotation(&sd, t + 3 * RATE_A_US / 4);
    }
    o->held = sd.held[pilot];
    o->held_src = sd.held[src];
}

/* The laps of a step run the pilot rang (one pass within 0.3 s of each, nothing else), as a bit per
 * lap at 5, 11, 17 and 23 s; bit 4: a pass far from every lap (a phantom). */
static unsigned step_laps(const step_out_t *o)
{
    unsigned bits = 0;
    for (unsigned i = 0; i < o->n; i++) {
        const int k = (int)lround((o->t[i] - 5.0) / 6.0);
        if (k >= 0 && k < 4 && fabs(o->t[i] - (5.0 + 6.0 * k)) < 0.3 && !(bits & (1u << k))) bits |= 1u << k;
        else bits |= 1u << 4;
    }
    return bits;
}

int main(void)
{
    static run_t r, q;   /* large: off the stack */

    /* 1. Defaults and the time constant. */
    const pd_config_t def = sd_config_default();
    CHECK(pd_config_valid(&def), "defaults are valid");
    start(&r, RATE_A_US, 0.0f, 1);
    printf("     tau_ms=%.2f\n", sd_tau_ms(&r.sd));
    CHECK(fabsf(sd_tau_ms(&r.sd) - 60.8f) <= 0.5f, "alpha 0.25 per 17.5 ms is tau 60.8 ms");
    {
        pd_config_t c = def;
        c.alpha = 0.5f;
        CHECK(sd_set_config(&r.sd, &c) && fabsf(sd_tau_ms(&r.sd) - 25.25f) < 0.05f, "sd_set_config recomputes tau");
        c.alpha = 0.0f;
        CHECK(!sd_set_config(&r.sd, &c) && r.sd.det.cfg.alpha == 0.5f, "sd_set_config refuses an invalid set");
        c.alpha = 1.0f;
        CHECK(sd_set_config(&r.sd, &c) && sd_tau_ms(&r.sd) == 0.0f, "alpha 1 is no smoothing");
    }

    /* 2. At 57 Hz the old weight per sweep, after the median's one sweep. */
    start(&r, RATE_B_US, 0.0f, 2);
    play(&r, 5.0, NULL);
    {
        const scene_t sc = {1, {rect(3, 4.0f, 5.0, 100.0)}};
        const float s0 = r.sd.c[3].smooth_db, r0 = sd_rise_db(&r.sd, 3);
        const float want[3] = {0.0f, 1.0f, 1.75f};
        bool smooth_ok = true, rise_ok = true;
        for (unsigned k = 0; k < 3u; k++) {
            rotation(&r, &sc);
            const float ds = r.sd.c[3].smooth_db - s0, dr = sd_rise_db(&r.sd, 3) - r0;
            printf("     step sweep %u: smooth +%.4f rise +%.4f (from %.4f)\n", k + 1u, ds, dr, r0);
            smooth_ok &= fabsf(ds - want[k]) <= 0.01f;
            rise_ok &= fabsf(sd_rise_db(&r.sd, 3) - want[k]) <= 0.01f;   /* noise-free: deterministic */
        }
        CHECK(smooth_ok, "a +4 dB step at 57 Hz: smoothed +0.00, +1.00, +1.75 after sweeps 1-3");
        CHECK(rise_ok, "and the smoothed rise reads 0.00, 1.00, 1.75");
    }

    /* 3. Quiet at 490 Hz, 10 minutes. */
    start(&r, RATE_A_US, NOISE_DB, 3);
    play(&r, 600.0, NULL);
    {
        const double mean = r.rise_sum / (double)r.rise_n;
        printf("     quiet: mean idle rise %.3f dB over %llu readings\n", mean, (unsigned long long)r.rise_n);
        CHECK(r.total == 0, "10 minutes of noise ring nothing");
        CHECK(mean >= -0.5 && mean <= 1.0, "the idle rise stays near 0");
    }

    /* 4. One pass at 490 Hz, 20 seeds. */
    pass_result_t a[20];
    {
        bool once = true, peak = true, err = true, lag = true;
        double err_sum = 0.0, peak_sum = 0.0;
        float err_worst = 0.0f;
        for (unsigned s = 0; s < 20u; s++) {
            a[s] = one_pass(RATE_A_US, 100u + s);
            once &= a[s].total == 1u && a[s].on_r4 == 1u;
            peak &= a[s].peak_db >= 10.0f && a[s].peak_db <= 16.5f;
            err &= fabsf(a[s].err_ms) <= 40.0f;
            lag &= a[s].lag_ms >= 20.0f && a[s].lag_ms <= 90.0f;
            err_sum += a[s].err_ms;
            peak_sum += a[s].peak_db;
            if (fabsf(a[s].err_ms) > fabsf(err_worst)) err_worst = a[s].err_ms;
        }
        printf("     pass: peak_db mean %.2f, estimate error mean %.1f ms worst %.1f ms, lag %.1f ms (seed 0)\n",
               peak_sum / 20.0, err_sum / 20.0, err_worst, a[0].lag_ms);
        CHECK(once, "a pass rings once, on R4, every seed");
        CHECK(peak, "with its rise (10..16.5 dB)");
        CHECK(err, "peak_ms - sd_delay_ms() is within 40 ms of the true centre, every seed");
        CHECK(fabs(err_sum / 20.0) <= 15.0, "and within 15 ms on average");
        CHECK(lag, "the uncorrected peak_ms lags the centre by 20..90 ms");
    }

    /* 5. The same pass at 57 Hz. */
    {
        bool once = true, close = true;
        double diff = 0.0;
        for (unsigned s = 0; s < 20u; s++) {
            const pass_result_t b = one_pass(RATE_B_US, 100u + s);
            once &= b.total == 1u && b.on_r4 == 1u;
            close &= fabsf(b.peak_db - a[s].peak_db) <= 1.5f;
            diff += b.peak_db - a[s].peak_db;
        }
        printf("     57 Hz: peak_db %+.2f dB from 490 Hz on average\n", diff / 20.0);
        CHECK(once, "at 57 Hz the pass rings once too");
        CHECK(close, "with a peak within 1.5 dB of the 490 Hz one, every seed");
    }

    /* 6. Two pilots side by side both count. */
    start(&r, RATE_A_US, NOISE_DB, 6);
    {
        const scene_t sc = {2, {gpass(1, 16.0f, 6.0, 150.0f), gpass(6, 14.0f, 6.0, 150.0f)}};
        play(&r, 10.0, &sc);
    }
    CHECK(r.total == 2 && r.passes[1] == 1 && r.passes[6] == 1, "two channels at once are two passes");

    /* 7. Nothing while the rests settle. */
    start(&r, RATE_A_US, NOISE_DB, 7);
    CHECK(sd_learning(&r.sd, T0_US - 1) && sd_learning(&r.sd, T0_US + S(100.0)), "learning before the first reading");
    {
        const scene_t sc = {1, {rect(2, 20.0f, 0.5, 2.0)}};
        play(&r, 2.9, &sc);
        CHECK(sd_learning(&r.sd, T0_US + S(3.0) - 1) && !sd_learning(&r.sd, T0_US + S(3.0)),
              "learning lasts 3 s from the first reading");
        play(&r, 8.0, &sc);
    }
    CHECK(r.total == 0, "a strong signal in the first 3 s rings nothing");

    /* 8. A VTX left on keeps showing (switched on after learning: its rest does not climb while it
     *    is active). A VTX on from the boot is test 18. */
    start(&r, RATE_A_US, NOISE_DB, 8);
    {
        const scene_t sc = {2, {rect(4, 15.0f, 5.0, 305.0), gpass(4, 15.0f, 309.0, 150.0f)}};
        play(&r, 305.0, &sc);                /* five minutes */
        CHECK(r.total == 0, "a steady VTX is no pass");
        CHECK(sd_rise_db(&r.sd, 4) > 13.0f, "it still shows after minutes");
        CHECK(sd_strongest(&r.sd) == 4, "as the strongest channel");
        float low = 1e9f;
        for (unsigned ch = 0; ch < PD_CHANNELS; ch++)
            if (ch != 4 && r.sd.c[ch].rest_db < low) low = r.sd.c[ch].rest_db;
        CHECK(r.sd.c[4].rest_db <= low + SD_REST_SPREAD_DB + 1.0f, "its rest does not climb while it is active");
        play(&r, 313.0, &sc);                /* it leaves (that rings), then one lap */
    }
    CHECK(r.passes[4] == 2 && r.total == 2, "its first lap counts");

    /* 9. Quick bumps of one pass stay one pass. */
    start(&r, RATE_A_US, NOISE_DB, 9);
    {
        scene_t sc = {0};
        for (unsigned k = 0; k < 4u; k++) {
            const double b = 5.0 + 0.315 * k;
            sc.b[sc.n++] = rect(0, 17.0f, b, b + 0.175);
            sc.b[sc.n++] = rect(0, 9.0f, b + 0.175, b + 0.315);
        }
        play(&r, 9.0, &sc);
    }
    CHECK(r.passes[0] == 1 && r.total == 1, "the refractory period holds a bumpy pass");

    /* 10. A weak bump never reaches the minimum peak, and its channel goes back to IDLE: the rest
     *     only climbs in IDLE, so a channel left RISING would stop following its floor. */
    {
        unsigned rang = 0;
        bool idle = true;
        for (unsigned s = 0; s < 5u; s++) {
            start(&r, RATE_A_US, NOISE_DB, 10u + s);
            const scene_t sc = {1, {rect(2, 9.0f, 5.0, 5.5)}};
            play(&r, 8.0, &sc);
            rang += r.total;
            idle &= r.sd.det.ch[2].state == PD_IDLE;
        }
        CHECK(rang == 0, "a +9 dB bump rings nothing (5 seeds)");
        CHECK(idle, "a +9 dB bump that never reached min_peak_db is released to IDLE (5 seeds)");
    }

    /* 11. A missed reading is no sample. */
    start(&r, RATE_A_US, NOISE_DB, 11);
    {
        scan_detector_t fresh;
        sd_init(&fresh, &def);
        pd_event_t e;
        CHECK(!sd_feed(&fresh, 0, SD_LEVEL_NONE, 0, T0_US, &e) && fresh.learn_end_us == 0 && !fresh.c[0].primed,
              "a missed first reading starts nothing");
        CHECK(!sd_feed(&fresh, PD_CHANNELS, qdb(QUIET_DB), 0, T0_US, &e) && fresh.learn_end_us == 0,
              "an out-of-range channel is ignored");
        play(&r, 5.0, NULL);
        const scan_detector_t before = r.sd;
        bool none = true, same = true;
        for (unsigned k = 0; k < 10u; k++) {
            for (unsigned ch = 0; ch < PD_CHANNELS; ch++)
                none &= !sd_feed(&r.sd, ch, SD_LEVEL_NONE, 0, r.t + (int64_t)(ch / 2u) * r.dt / 4, &e);
            sd_rotation(&r.sd, r.t + 3 * r.dt / 4);
            r.t += r.dt;
        }
        for (unsigned ch = 0; ch < PD_CHANNELS; ch++)
            same &= same_channel(&before.c[ch], &r.sd.c[ch]) && before.det.ch[ch].smooth == r.sd.det.ch[ch].smooth &&
                    before.det.ch[ch].state == r.sd.det.ch[ch].state;
        CHECK(none && same, "missed readings change neither the rests nor the smoothing");
    }

    /* 12. A lone burst reading (Wi-Fi) vanishes in the median. */
    {
        start(&r, RATE_A_US, NOISE_DB, 12);      /* burst-free reference, same noise */
        play(&r, 60.0, NULL);
        start(&q, RATE_A_US, NOISE_DB, 12);
        while (q.t < T0_US + S(60.0)) {
            const bool burst = q.reads[2] % 25u == 24u;   /* every 25th reading of R3 */
            const scene_t sc = {1, {rect(2, burst ? 30.0f : 0.0f, -1.0, 1e6)}};
            rotation(&q, &sc);
        }
        printf("     bursts: R3 max rise %.3f dB, %.3f without bursts\n", q.rise_max[2], r.rise_max[2]);
        CHECK(q.total == 0, "a +30 dB burst every 25th reading rings nothing");
        CHECK(fabsf(q.rise_max[2] - r.rise_max[2]) < 0.2f, "and moves the smoothed rise by under 0.2 dB");

        start(&q, RATE_A_US, NOISE_DB, 12);
        float pre = 0.0f;
        while (q.t < T0_US + S(20.0)) {
            const uint32_t k = q.reads[2];
            if (k == 3000u) {
                pre = sd_rise_db(&q.sd, 2);
                reset_max(&q);
            }
            const scene_t sc = {1, {rect(2, k == 3000u || k == 3001u ? 30.0f : 0.0f, -1.0, 1e6)}};
            rotation(&q, &sc);
        }
        printf("     adjacent bursts: R3 rise %.3f -> max %.3f dB\n", pre, q.rise_max[2]);
        CHECK(q.rise_max[2] - pre > 1.5f && q.rise_max[2] - pre < 2.5f, "two adjacent bursts pass the median twice");
        CHECK(q.rise_max[2] < 3.0f && q.total == 0, "and the smoothed rise stays under 3 dB");
    }

    /* 13. A gap (a calibration) neither rings nor inflates the delay estimate. */
    start(&r, RATE_A_US, NOISE_DB, 13);
    {
        const scene_t sc = {1, {rect(5, 15.0f, 5.0, 10.0)}};
        play(&r, 10.0, &sc);
        float rest[PD_CHANNELS];
        for (unsigned ch = 0; ch < PD_CHANNELS; ch++) rest[ch] = r.sd.c[ch].rest_db;
        r.t += S(2.0);                       /* 2 s without readings */
        bool dt_ok = true;
        float moved = 0.0f;
        for (unsigned k = 0; k < 10u; k++) {
            rotation(&r, &sc);
            dt_ok &= r.sd.c[5].dt_avg_us > 0 && r.sd.c[5].dt_avg_us < 3000;
            if (k == 0)
                for (unsigned ch = 0; ch < PD_CHANNELS; ch++)
                    if (fabsf(r.sd.c[ch].rest_db - rest[ch]) > moved) moved = fabsf(r.sd.c[ch].rest_db - rest[ch]);
        }
        printf("     gap: rests moved by %.4f dB at most\n", moved);
        CHECK(moved <= SD_REST_DOWN_DB * (float)SD_DT_MAX_US / (float)SD_REF_SWEEP_US + 0.01f,
              "the gap moves no rest by more than SD_DT_MAX_US of descent");
        play(&r, 17.0, &sc);
        printf("     gap: R6 dt_avg_us %ld after\n", (long)r.sd.c[5].dt_avg_us);
        CHECK(r.total == 0, "a VTX gone during a 2 s gap rings nothing");
        CHECK(dt_ok, "the reading interval stays under 3 ms after the gap");
        r.t += S(0.09);                      /* a 90 ms hiccup: no re-prime, one late reading */
        rotation(&r, &sc);
        printf("     hiccup: R6 dt_avg_us %ld\n", (long)r.sd.c[5].dt_avg_us);
        CHECK(r.sd.c[5].dt_avg_us < 3000, "one late reading barely moves the reading interval");
    }

    /* 14. A saturated capture never moves a rest. */
    {
        start(&r, RATE_A_US, NOISE_DB, 14);
        play(&r, 5.0, NULL);
        const float rest = r.sd.c[1].rest_db;
        r.sat_mask = 1u << 1;
        const scene_t sc = {1, {rect(1, -3.0f, 5.0, 65.0)}};
        play(&r, 65.0, &sc);
        start(&q, RATE_A_US, NOISE_DB, 14);
        play(&q, 5.0, NULL);
        const float rest_q = q.sd.c[1].rest_db;
        play(&q, 65.0, &sc);
        printf("     saturated: R2 rest %.3f -> %.3f flagged, %.3f -> %.3f not\n", rest, r.sd.c[1].rest_db, rest_q,
               q.sd.c[1].rest_db);
        CHECK(r.sd.c[1].rest_db == rest, "60 s of saturated readings at -3 dB leave the rest");
        CHECK(q.sd.c[1].rest_db < rest_q - 2.0f, "unflagged, they pull it down");
    }

    /* 15. The median's history never wraps. */
    start(&r, RATE_A_US, 0.0f, 15);
    r.only_mask = 1u << 0;
    play(&r, 5.0, NULL);                     /* past learning */
    reset_max(&r);
    for (uint32_t i = 0; i < 100000u; i++) {
        const bool burst = r.reads[0] % 256u == 0u;
        const scene_t sc = {1, {rect(0, burst ? 30.0f : 0.0f, -1.0, 1e6)}};
        rotation(&r, &sc);
    }
    printf("     wrap: max rise %.4f dB over %lu readings\n", r.rise_max[0], (unsigned long)r.reads[0]);
    CHECK(r.rise_max[0] < 0.2f && r.total == 0, "a burst every 256 readings never passes the median");

    /* 16. A lone burst as the first reading after a gap is still a lone burst: the channel waits
     *     for a full median. A gap under pd's stale_ms (a PHY-tracking recalibration, ~0.2 s) and
     *     a longer one. */
    {
        const struct { float db; double gap_s; } cases[4] = {{30.0f, 0.2}, {15.0f, 0.2}, {30.0f, 1.5}, {15.0f, 1.5}};
        unsigned rang = 0;
        for (unsigned k = 0; k < 4u; k++) {
            start(&r, RATE_A_US, NOISE_DB, 160u + k);
            play(&r, 10.0, NULL);
            r.t += S(cases[k].gap_s);
            const scene_t burst = {1, {{.ch = 2, .db = cases[k].db, .from = r.t, .to = r.t + r.dt / 2}}};
            rotation(&r, &burst);            /* R3's first reading after the gap */
            play(&r, 14.0, NULL);
            rang += r.total;
        }
        CHECK(rang == 0, "a +15/+30 dB burst as the first reading after a 0.2/1.5 s gap rings nothing");
    }

    /* 17. A parked VTX across 0.2 s gaps (PHY-tracking recalibrations): the average resumes where
     *     it was, so neither a high nor a low first median after a gap rings against the peak
     *     held from before. 2000 gaps, 1.3 s apart; re-priming at every gap rang on ~0.2 % of
     *     them, and only the first phantom would show (the channel then stays in cooldown). */
    start(&r, RATE_A_US, NOISE_DB, 17);
    {
        const scene_t sc = {1, {rect(4, 15.0f, 5.0, 1e6)}};
        play(&r, 8.0, &sc);
        for (unsigned g = 0; g < 2000u; g++) {
            r.t += S(0.2);
            play(&r, 8.0 + 1.5 * (g + 1u), &sc);
        }
        printf("     parked VTX across 2000 gaps: %u passes, R5 %s\n", r.total, pd_state_name(r.sd.det.ch[4].state));
        CHECK(r.total == 0 && r.sd.det.ch[4].state == PD_RISING, "a VTX parked across 2000 gaps of 0.2 s rings nothing");
    }

    /* 18. A drone waiting by the gate from the boot is not learnt as its channel's rest: the rest
     *     stops 6 dB over the others' median, the VTX still shows, and its first lap counts. */
    start(&r, RATE_A_US, NOISE_DB, 18);
    {
        const scene_t sc = {2, {rect(4, 15.0f, -1.0, 60.0), gpass(4, 15.0f, 75.0, 150.0f)}};
        play(&r, 60.0, &sc);
        printf("     VTX from boot: R5 rest %.2f dB (cap %.2f), rise %.2f dB\n", r.sd.c[4].rest_db, r.sd.cap_db,
               sd_rise_db(&r.sd, 4));
        CHECK(r.sd.c[4].rest_db <= r.sd.cap_db + 0.25f, "its rest is capped at the others' median + 6 dB");
        CHECK(sd_rise_db(&r.sd, 4) > r.sd.det.cfg.active_db && r.total == 0, "it still shows, above active_db, and rings nothing");
        play(&r, 80.0, &sc);                 /* it leaves at 60 s, then one lap at 75 s */
    }
    CHECK(r.passes[4] == 1 && r.total == 1, "its first lap counts");

    /* sd_relearn starts over. */
    sd_relearn(&r.sd);
    CHECK(sd_learning(&r.sd, r.t) && !r.sd.c[0].rest_primed && !r.sd.det.ch[0].primed, "sd_relearn forgets the rests");

    /* 19. A late reading (a gap under SD_DT_MAX_US: no median restart) weighs as one on time:
     *     noise-free, R6 steps +6 dB 50 ms before a 90 ms gap; the first reading after it moves
     *     the average by one interval's weight (~0.2 dB), not the gap's (~2.1 dB). */
    start(&r, RATE_A_US, 0.0f, 1);
    {
        const scene_t sc = {1, {rect(5, 6.0f, 9.95, 1e6)}};
        play(&r, 10.0, &sc);
        const float before = r.sd.c[5].smooth_db;
        r.t += S(0.09);
        rotation(&r, &sc);
        const float step = r.sd.c[5].smooth_db - before;
        printf("     late reading after 90 ms: R6 average +%.3f dB\n", step);
        CHECK(step > 0.0f && step < 0.5f, "a late reading moves the average by one interval's weight, not the gap's");
    }

    /* 20. Test 17 with gaps under SD_DT_MAX_US (90 ms: a stale re-tune, a PHY-tracking
     *     recalibration): the median is not restarted, and the late reading's weight is bounded,
     *     so a parked VTX still rings nothing. 2000 gaps, 1.3 s apart. */
    start(&r, RATE_A_US, NOISE_DB, 171);
    {
        const scene_t sc = {1, {rect(4, 15.0f, 5.0, 1e6)}};
        play(&r, 8.0, &sc);
        for (unsigned g = 0; g < 2000u; g++) {
            r.t += S(0.09);
            play(&r, 8.0 + 1.3 * (g + 1u), &sc);
        }
        printf("     parked VTX across 2000 gaps of 90 ms: %u passes, R5 %s\n", r.total, pd_state_name(r.sd.det.ch[4].state));
        CHECK(r.total == 0 && r.sd.det.ch[4].state == PD_RISING, "a VTX parked across 2000 gaps of 90 ms rings nothing");
    }

    /* 21. A pass rings on R6, the drone then hovers by the gate (+13 dB, so the channel holds in
     *     COOLDOWN), and R5/R6 get no reading for 1.2 s (their LO's captures fail: past stale_ms,
     *     without a relearn). The kept cooldown holds when readings resume: when the drone
     *     leaves, nothing rings a second time. */
    start(&r, RATE_A_US, NOISE_DB, 173);
    {
        const scene_t sc = {2, {gpass(5, 25.0f, 6.0, 120.0f), rect(5, 13.0f, 6.4, 20.0)}};
        play(&r, 9.0, &sc);
        const unsigned rang = r.total;
        r.only_mask = 0xCF;                  /* R5 and R6 unread */
        play(&r, 10.2, &sc);
        r.only_mask = 0;
        play(&r, 25.0, &sc);
        printf("     hover across a 1.2 s reading gap: %u pass(es), %u before it\n", r.total, rang);
        CHECK(rang == 1 && r.passes[5] == 1 && r.total == 1, "a drone hovering across a 1.2 s reading gap rings once");
    }

    /* 22. Pair guard: without a saturated capture, sd_feed_hop is sd_feed of each reading, to the
     *     bit (passes on both channels of a hop, 10 dB apart, and on others). */
    {
        static scan_detector_t a, b;
        const pd_config_t c = sd_config_default();
        sd_init(&a, &c);
        sd_init(&b, &c);
        seed(181);
        const scene_t sc = {4, {gpass(4, 30.0f, 6.0, 80.0f), gpass(5, 40.0f, 6.05, 60.0f), gpass(1, 20.0f, 8.0, 150.0f),
                                rect(5, 12.0f, 9.0, 11.0)}};
        unsigned ea = 0, eb = 0, diff = 0;
        for (int64_t t = T0_US; t < T0_US + S(14.0); t += RATE_A_US) {
            for (unsigned hop = 0; hop < 4u; hop++) {
                const int64_t th = t + (int64_t)hop * RATE_A_US / 4;
                const sd_reading_t rd[2] = {
                    {th, qdb(QUIET_DB + scene_db(&sc, 2u * hop, th) + gauss(NOISE_DB)), (uint8_t)(2u * hop), 0},
                    {th, qdb(QUIET_DB + scene_db(&sc, 2u * hop + 1u, th) + gauss(NOISE_DB)), (uint8_t)(2u * hop + 1u), 0}};
                pd_event_t e1[2], e2[2];
                unsigned rang = 0, held = 1;
                for (unsigned i = 0; i < 2; i++)
                    if (sd_feed(&a, rd[i].channel, rd[i].level_qdb, rd[i].flags, rd[i].t_us, &e1[i])) rang |= 1u << i;
                const unsigned rang2 = sd_feed_hop(&b, hop, rd, 2, e2, &held);
                diff += rang != rang2 || held;
                for (unsigned i = 0; i < 2; i++)
                    if (rang & rang2 & (1u << i)) {
                        ea++;
                        diff += e1[i].channel != e2[i].channel || e1[i].peak_db != e2[i].peak_db ||
                                e1[i].peak_ms != e2[i].peak_ms || e1[i].at_ms != e2[i].at_ms;
                    }
                eb += (unsigned)__builtin_popcount(rang2);
            }
            sd_rotation(&a, t + 3 * RATE_A_US / 4);
            sd_rotation(&b, t + 3 * RATE_A_US / 4);
        }
        bool same = true;
        for (unsigned ch = 0; ch < PD_CHANNELS; ch++)
            same = same && same_channel(&a.c[ch], &b.c[ch]) && !memcmp(&a.det.ch[ch], &b.det.ch[ch], sizeof(a.det.ch[ch]));
        printf("     clean pairs: %u passes per reading, %u per hop, %u differences\n", ea, eb, diff);
        CHECK(ea >= 3 && eb == ea && diff == 0 && same, "without a saturated capture sd_feed_hop is sd_feed, to the bit");
    }

    /* 23. The bench's ghost: a 600 mW R6 passing 0.3 m from the gate clips the ADC for ~80 ms; R5,
     *     alone in the air, reads R6's clipping products. Fed one reading at a time (cp_scan.c until
     *     now) R5 rings a phantom; through the pair guard it is held while R6 clips and rings
     *     nothing, and R6's own pass is the same pass to the millisecond (R6 is never held). */
    {
        static scan_detector_t sd;
        const pdrone_t r6 = {.mw = 600, .d_m = 0.3, .t0_s = 2.0}, none = {0};
        unsigned ghost_base = 0, ghost_guard = 0, r6_same = 0, r6_held = 0;
        float ghost_db = 0.0f;
        for (uint64_t k = 0; k < 10u; k++) {
            pair_out_t base, gd;
            pair_run(&r6, &none, 0, 300 + k, 4.0, &base, &sd);
            pair_run(&r6, &none, 1, 300 + k, 4.0, &gd, &sd);
            ghost_base += base.n[PAIR_LO] > 0;
            ghost_guard += gd.n[PAIR_LO] > 0;
            if (base.n[PAIR_LO] && base.peak_db[PAIR_LO][0] > ghost_db) ghost_db = base.peak_db[PAIR_LO][0];
            r6_same += base.n[PAIR_HI] == 1 && gd.n[PAIR_HI] == 1 && base.peak_s[PAIR_HI][0] == gd.peak_s[PAIR_HI][0];
            r6_held += gd.held[PAIR_HI];
        }
        printf("     600 mW at 0.3 m: R5 phantom in %u/10 runs fed per reading (up to %.1f dB), %u/10 guarded\n", ghost_base,
               (double)ghost_db, ghost_guard);
        CHECK(ghost_base >= 9 && ghost_db >= 13.0f, "fed per reading, a clipping R6 rings a phantom R5 pass (the bench's ghost)");
        CHECK(ghost_guard == 0, "through sd_feed_hop it rings none");
        CHECK(r6_same == 10 && r6_held == 0, "and R6's pass is unchanged: the strong channel is never held");
    }

    /* 24. A real R5 pilot 30 dB under that R6, on the same line, 200 ms behind it, then at the same
     *     instant. Per reading, the phantom at R6's time takes the pass and its refractory kills the
     *     real one; guarded, R5 rings once, on its own pass, within 30 ms (200 ms) or 80 ms (same
     *     instant: blind while R6 clips) of R5 alone in the air. */
    {
        static scan_detector_t sd;
        const pdrone_t r6 = {.mw = 600, .d_m = 0.3, .t0_s = 2.0}, none = {0};
        unsigned base_ok = 0, guard_ok = 0, same_ok = 0;
        double worst = 0.0, worst_same = 0.0;
        for (uint64_t k = 0; k < 10u; k++) {
            const pdrone_t r5 = {.mw = 0.6, .d_m = 0.3, .t0_s = 2.2}, r5s = {.mw = 0.6, .d_m = 0.3, .t0_s = 2.0};
            pair_out_t base, gd, alone, gs, alone_s;
            pair_run(&r6, &r5, 0, 400 + k, 4.0, &base, &sd);
            pair_run(&r6, &r5, 1, 400 + k, 4.0, &gd, &sd);
            pair_run(&none, &r5, 0, 400 + k, 4.0, &alone, &sd);
            pair_run(&r6, &r5s, 1, 500 + k, 4.0, &gs, &sd);
            pair_run(&none, &r5s, 0, 500 + k, 4.0, &alone_s, &sd);
            base_ok += base.n[PAIR_LO] == 1 && fabs(base.peak_s[PAIR_LO][0] - alone.peak_s[PAIR_LO][0]) <= 0.03;
            if (gd.n[PAIR_LO] == 1 && alone.n[PAIR_LO] == 1) {
                guard_ok++;
                worst = fmax(worst, fabs(gd.peak_s[PAIR_LO][0] - alone.peak_s[PAIR_LO][0]));
            }
            if (gs.n[PAIR_LO] == 1 && alone_s.n[PAIR_LO] == 1) {
                same_ok++;
                worst_same = fmax(worst_same, fabs(gs.peak_s[PAIR_LO][0] - alone_s.peak_s[PAIR_LO][0]));
            }
        }
        printf("     R5 30 dB under, +200 ms: %u/10 right per reading, %u/10 guarded (worst %.0f ms); same instant %u/10 (worst %.0f ms)\n",
               base_ok, guard_ok, worst * 1e3, same_ok, worst_same * 1e3);
        CHECK(base_ok <= 2, "per reading, the phantom replaces a real R5 pass 200 ms behind a clipping R6");
        CHECK(guard_ok == 10 && worst <= 0.03, "guarded, R5 rings its own pass, within 30 ms of R5 alone");
        CHECK(same_ok == 10 && worst_same <= 0.08, "at the same instant too, within 80 ms (blind while R6 clips)");
    }

    /* 25. A 600 mW R6 parked 0.5 m from the gate for 10 s (clipping) while R5 laps at 1 m every 3 s:
     *     R5 is held throughout (unmeasurable under the clipping products) and rings nothing, no
     *     phantom and no mistimed lap; when R6 leaves, R5 resumes in COOLDOWN, so the tail of the
     *     lap it missed at 10 s does not ring either. */
    {
        static scan_detector_t sd;
        const pdrone_t r6 = {.mw = 600, .hover_d_m = 0.5, .hover_t0_s = 0.5, .hover_t1_s = 10.5};
        const pdrone_t r5 = {.mw = 25, .d_m = 1.0, .t0_s = 1.0, .laps = 4, .lap_s = 3.0};
        unsigned rang = 0, held_ok = 0;
        for (uint64_t k = 0; k < 5u; k++) {
            pair_out_t gd;
            pair_run(&r6, &r5, 1, 600 + k, 12.0, &gd, &sd);
            rang += gd.n[PAIR_LO];
            /* sd.held, the per-channel count the console prints (K l, PASS), is the bits returned. */
            held_ok += gd.held[PAIR_LO] > 4000u && gd.held[PAIR_HI] == 0u && sd.held[PAIR_LO] == gd.held[PAIR_LO] &&
                       sd.held[PAIR_HI] == 0u;
            for (unsigned i = 0; i < gd.n[PAIR_LO]; i++) printf("     (R5 pass at %.3f s, %.1f dB; held %u)\n", gd.peak_s[PAIR_LO][i], (double)gd.peak_db[PAIR_LO][i], gd.held[PAIR_LO]);
        }
        printf("     10 s clipping hover: %u R5 passes over 5 runs\n", rang);
        CHECK(rang == 0 && held_ok == 5, "a clipping hover blinds its partner: no lap, no phantom, no tail");
    }

    /* 26. Known loss, kept as it is: a slow co-pass. The 600 mW R6 of 23-24 passes 0.3 m away at
     *     5 m/s, so it clips for about 320 ms instead of 80; a real R5 20 or 30 dB under it crosses at
     *     the same instant, on the same line. Guarded, R5 is blind for that whole window, which holds
     *     its own peak: it rings once, but timed at an edge of the window (its last readings before
     *     the hold, or its first after it), 0.17-0.22 s from R5 alone in the air. Per reading it looks
     *     right only because its phantom peaks at R6's instant, which is R5's here (at another
     *     offset the phantom takes the pass: 24). The host model of the eval saw the same (5 of 20
     *     on time at 5 m/s); the window grows as 1/speed and with the link gain (Kconfig
     *     CHECKPOINT_PAIR_GUARD). Bench B11 measures it on the board; retune from that, not here. */
    {
        static scan_detector_t sd;
        const pdrone_t r6 = {.mw = 600, .d_m = 0.3, .t0_s = 2.0, .v_ms = 5.0}, none = {0};
        unsigned once = 0, base_ok = 0, r6_same = 0, runs = 0;
        double lo = 1e9, hi = 0.0;
        for (int dl = 20; dl <= 30; dl += 10) {
            const pdrone_t r5 = {.mw = 600.0 * pow(10.0, -dl / 10.0), .d_m = 0.3, .t0_s = 2.0, .v_ms = 5.0};
            for (uint64_t k = 0; k < 10u; k++) {
                pair_out_t base, gd, alone;
                pair_run(&r6, &r5, 0, 700 + k, 4.0, &base, &sd);
                pair_run(&r6, &r5, 1, 700 + k, 4.0, &gd, &sd);
                pair_run(&none, &r5, 0, 700 + k, 4.0, &alone, &sd);
                runs++;
                r6_same += base.n[PAIR_HI] == 1 && gd.n[PAIR_HI] == 1 && base.peak_s[PAIR_HI][0] == gd.peak_s[PAIR_HI][0];
                if (alone.n[PAIR_LO] != 1) continue;
                base_ok += base.n[PAIR_LO] == 1 && fabs(base.peak_s[PAIR_LO][0] - alone.peak_s[PAIR_LO][0]) <= 0.1;
                if (gd.n[PAIR_LO] == 1) {
                    once++;
                    const double err = fabs(gd.peak_s[PAIR_LO][0] - alone.peak_s[PAIR_LO][0]);
                    lo = fmin(lo, err);
                    hi = fmax(hi, err);
                }
            }
        }
        printf("     5 m/s co-pass, R5 20/30 dB under: guarded %u/%u ring once, %.0f-%.0f ms from R5 alone; per reading "
               "%u/%u within 100 ms\n", once, runs, lo * 1e3, hi * 1e3, base_ok, runs);
        CHECK(once == runs && lo >= 0.12 && hi <= 0.30,
              "known loss: a 5 m/s co-pass rings once, at an edge of the ~320 ms clip window (0.12-0.30 s off)");
        CHECK(base_ok == runs && r6_same == runs, "per reading it rings near R5's time (its phantom's), and R6 is the same");
    }

    /* 27. Relative re-arm at the reading rate (490 Hz, median, average, rests): a VTX on R3 that
     *     stays 28 dB up from 4 s, with passes 20 dB over that at 5, 15, 25 and 35 s. Off, only the
     *     first rings (the bench's one 22.5 s pass); at 16 dB, all four. While relatively armed the
     *     channel is not quiet: its rest does not climb (it would, by 0.114 dB/s, in plain IDLE). */
    {
        const scene_t sc = {5, {rect(2, 28.0f, 4.0, 40.0), gpass(2, 20.0f, 5.0, 250.0f), gpass(2, 20.0f, 15.0, 250.0f),
                               gpass(2, 20.0f, 25.0, 250.0f), gpass(2, 20.0f, 35.0, 250.0f)}};
        s_rearm_db = 0.0f;
        start(&r, RATE_A_US, NOISE_DB, 27);
        play(&r, 40.0, &sc);
        const unsigned off = r.total;
        s_rearm_db = 16.0f;
        start(&r, RATE_A_US, NOISE_DB, 27);
        play(&r, 9.0, &sc);
        const bool armed = r.sd.det.ch[2].state == PD_IDLE && r.sd.det.ch[2].rel;
        const float rest9 = r.sd.c[2].rest_db;
        play(&r, 14.0, &sc);
        const float rest14 = r.sd.c[2].rest_db;
        play(&r, 40.0, &sc);
        printf("     VTX 28 dB up between passes: %u pass(es) without the re-arm, %u with 16 dB; rest %+.3f dB over 5 s armed\n",
               off, r.total, (double)(rest14 - rest9));
        CHECK(off == 1 && r.total == 4 && r.passes[2] == 4 && r.last.rel, "a VTX staying 28 dB up: 1 pass without the re-arm, all 4 with 16 dB");
        CHECK(armed && rest14 == rest9, "relatively armed, the rest does not climb");
        s_rearm_db = 0.0f;
    }

    /* 28. The pair guard's long hold and the relative re-arm. R5 passes close (45 dB up at 4.5 s) and
     *     stays 25 dB up, so it re-arms relatively; then a 600 mW R6 parks clipping beside the gate
     *     for 0.8 s (over SD_HOLD_LONG_US, under stale_ms) and holds it. R5 resumes in COOLDOWN with
     *     its reference at the level it resumes at: it does not re-arm at once on the old peak
     *     (COOLDOWN 4 s later, no R5 pass). Then 25 again with the re-arm on: no R5 lap. */
    {
        static scan_detector_t sd;
        unsigned ok = 0, armed = 0, rang = 0;
        for (uint64_t k = 0; k < 5u; k++) {
            bool probed = false;
            const pd_config_t c = sd_config_default();
            sd_init(&sd, &c);
            pd_set_rearm(&sd.det, 16.0f);
            seed(880 + k);
            for (int64_t t = T0_US; t < T0_US + S(13.0); t += RATE_A_US) {
                for (unsigned hop = 0; hop < 4u; hop++) {
                    const int64_t th = t + (int64_t)hop * RATE_A_US / 4;
                    const double ts = (double)(th - T0_US) * 1e-6;
                    double ra = PAIR_FLOOR + gauss(1.2f), rb = PAIR_FLOOR + gauss(1.2f);
                    uint8_t fl = 0;
                    if (hop == PAIR_HOP && ts >= 4.0) {
                        const double x = (ts - 4.5) / 0.15;
                        ra = psum(ra, PAIR_FLOOR + 25.0 + 20.0 * exp(-0.5 * x * x) + gauss(1.0f));
                        /* R6 flies in over 0.5 s, clips 8-8.8 s (R5 reads its products: held), flies off */
                        const double in = ts < 8.0 ? 8.0 - ts : ts > 8.8 ? ts - 8.8 : 0.0;
                        if (in == 0.0) {
                            fl = SD_FLAG_SATURATED;
                            rb = PAIR_CLIP_DBFS + 5.0 + gauss(0.3f);
                            ra = psum(ra, rb + clip_x());
                        } else if (in < 0.5) {
                            rb = psum(rb, PAIR_CLIP_DBFS - 1.0 - 60.0 * in + gauss(1.0f));
                        }
                    }
                    if (hop == PAIR_HOP && ts >= 7.9 && !probed) {   /* before R6 comes in */
                        probed = true;
                        armed += sd.det.ch[PAIR_LO].state == PD_IDLE && sd.det.ch[PAIR_LO].rel;
                    }
                    const sd_reading_t rd[2] = {{th, qdb((float)ra), (uint8_t)(2u * hop), fl},
                                                {th, qdb((float)rb), (uint8_t)(2u * hop + 1u), fl}};
                    pd_event_t ev[2];
                    const unsigned got = sd_feed_hop(&sd, hop, rd, 2, ev, NULL);
                    if ((got & 1u) && hop == PAIR_HOP && ts > 5.0) rang++;
                }
                sd_rotation(&sd, t + 3 * RATE_A_US / 4);
            }
            ok += sd.det.ch[PAIR_LO].state == PD_COOLDOWN && !sd.det.ch[PAIR_LO].rel && sd.held[PAIR_LO] > 300u;
        }
        CHECK(armed == 5 && ok == 5 && rang == 0,
              "after a long hold the reference restarts from the level R5 resumes at: no re-arm on the old peak");
        s_rearm_db = 16.0f;
        const pdrone_t r6 = {.mw = 600, .hover_d_m = 0.5, .hover_t0_s = 0.5, .hover_t1_s = 10.5};
        const pdrone_t r5 = {.mw = 25, .d_m = 1.0, .t0_s = 1.0, .laps = 4, .lap_s = 3.0};
        rang = 0;
        for (uint64_t k = 0; k < 5u; k++) {
            pair_out_t gd;
            pair_run(&r6, &r5, 1, 600 + k, 12.0, &gd, &sd);
            rang += gd.n[PAIR_LO];
        }
        CHECK(rang == 0, "check 25 with the relative re-arm on: still no R5 lap, no phantom, no tail");
        s_rearm_db = 0.0f;
    }

    /* 29. A step onset: a VTX switched on or armed beside the antenna clips at once, while its average
     *     is still at the floor. R5 laps (5, 11, 17, 23 s), 18 or 25 dB up between laps (relatively
     *     re-armed) or at the floor; R6 clips from 13.5 s for 0.1 or 0.3 s, switched on and off at
     *     once. By the smoothed levels alone, with R5 up, R6 was held for the whole run (its average
     *     still low) and R5 fed R6's products; with R5 at the floor, R5 was fed them until R6's average
     *     caught up: a phantom R5 pass in 44 of these 120 runs (all 40 at the floor) and no R6 pass in
     *     80. Once R6's last 3 readings show it is the source, R5 is held
     *     until the run ends: R6 is held on its first clipped captures only (2 mostly, 5 at most here,
     *     none with R5 at the floor), R5 rings its four laps and nothing else, and R6 rings its own
     *     pass. Mirrored (R5 switched on, R6 lapping) the same. Flown in over 0.5 s, R6 is never held,
     *     as before. */
    {
        unsigned runs = 0, ok_laps = 0, ok_src = 0, ok_held = 0, worst_src = 0;
        static const double bases[3] = {18.0, 25.0, 0.0}, clips[2] = {0.1, 0.3};
        for (int mirror = 0; mirror < 2; mirror++)
            for (unsigned b = 0; b < 3u; b++)
                for (unsigned c = 0; c < 2u; c++)
                    for (uint64_t k = 0; k < 10u; k++) {
                        const step_t p = {.base = bases[b], .after = bases[b], .clip_s = clips[c], .mirror = mirror, .rearm = 16.0f};
                        step_out_t o;
                        step_run(&p, 2900 + 10u * b + k, &o);
                        runs++;
                        ok_laps += step_laps(&o) == 0xFu;
                        ok_src += o.n_src == 1u;
                        ok_held += o.held_src <= 8u && o.held + 8u >= (unsigned)(clips[c] * 490.0);   /* 8 captures at most */
                        if (o.held_src > worst_src) worst_src = o.held_src;
                    }
        unsigned ramp_ok = 0;
        for (uint64_t k = 0; k < 10u; k++) {
            const step_t p = {.base = 25.0, .after = 25.0, .clip_s = 0.3, .ramp = 1, .rearm = 16.0f};
            step_out_t o;
            step_run(&p, 2990 + k, &o);
            ramp_ok += o.held_src == 0u && o.held > 140u && step_laps(&o) == 0xFu && o.n_src == 1u;
        }
        printf("     step onset, %u runs: all laps and no phantom in %u, the source's pass in %u, held right in %u "
               "(the source at most %u readings); flown in: %u/10\n", runs, ok_laps, ok_src, ok_held, worst_src, ramp_ok);
        CHECK(ok_laps == runs && ok_src == runs && ok_held == runs,
              "a step into the rails: its partner is held, every lap kept, no phantom, the source rings its pass");
        CHECK(ramp_ok == 10u, "flown in, the smoothed levels hold the partner as before (the source never held)");
    }

    printf("%d/%d scan_detector checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
