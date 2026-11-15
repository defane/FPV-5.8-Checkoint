/* Host test for components/pass_detector's scan detector, fed per-channel readings at the
 * sdr_rx rate (490 Hz per channel, R1..R8 in LO pairs) and at the old 57 Hz sweep.
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

    /* 8. A VTX left on keeps showing, and a drone waiting by the gate is not learnt. */
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
        CHECK(r.sd.c[4].rest_db <= low + SD_REST_SPREAD_DB + 1.0f, "its rest stays near the others'");
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

    /* 10. A weak bump never reaches the minimum peak. */
    {
        unsigned rang = 0;
        for (unsigned s = 0; s < 5u; s++) {
            start(&r, RATE_A_US, NOISE_DB, 10u + s);
            const scene_t sc = {1, {rect(2, 9.0f, 5.0, 5.5)}};
            play(&r, 8.0, &sc);
            rang += r.total;
        }
        CHECK(rang == 0, "a +9 dB bump rings nothing (5 seeds)");
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
        r.t += S(2.0);                       /* 2 s without readings */
        bool dt_ok = true;
        for (unsigned k = 0; k < 10u; k++) {
            rotation(&r, &sc);
            dt_ok &= r.sd.c[5].dt_avg_us > 0 && r.sd.c[5].dt_avg_us < 3000;
        }
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

    /* sd_relearn starts over. */
    sd_relearn(&r.sd);
    CHECK(sd_learning(&r.sd, r.t) && !r.sd.c[0].rest_primed && !r.sd.det.ch[0].primed, "sd_relearn forgets the rests");

    printf("%d/%d scan_detector checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
