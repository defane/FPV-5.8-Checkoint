/* Host test of the checkpoint pass detector (components/pass_detector) on
 * synthetic C/N traces sampled at 20 Hz, like the firmware's 50 ms tick.
 *
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
 *      test_pass_detector.c ../components/pass_detector/pass_detector.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "pass_detector.h"

#define TICK_MS 50u
#define FLOOR_DB 8.0f

static unsigned s_fail, s_run;
static void check(const char *name, int ok)
{
    ++s_run;
    printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
    s_fail += !ok;
}

/* Deterministic noise: LCG, approximately Gaussian (sum of four uniforms). */
static uint32_t s_seed;
static float uniform(void) { s_seed = s_seed * 1664525u + 1013904223u; return (float)(s_seed >> 8) / 16777216.0f; }
static float gauss(float sigma) { return sigma * (uniform() + uniform() + uniform() + uniform() - 2.0f) * 1.732f; }

/* A straight fly-by: free-space level over the closest approach d0 at speed
 * v, peak `peak_db` at t0, never under the noise floor. */
static float pass_db(float t_s, float t0_s, float peak_db, float d0_m, float v_mps)
{
    const float x = v_mps * (t_s - t0_s) / d0_m;
    const float db = peak_db - 10.0f * log10f(1.0f + x * x);
    return db > FLOOR_DB ? db : FLOOR_DB;
}

typedef struct { unsigned n; pd_event_t ev[8]; } events_t;

static void feed(pass_detector_t *d, unsigned ch, float db, uint32_t now, events_t *out)
{
    pd_event_t e;
    if (pd_update(d, ch, db, now, &e) && out->n < 8u) out->ev[out->n++] = e;
}

/* One drone on channel ch over `seconds`; times from t_start_ms. */
static events_t run_pass(float peak_db, float sigma, uint32_t seed, unsigned ch, uint32_t t_start_ms)
{
    pass_detector_t d;
    pd_config_t cfg = pd_config_default();
    pd_init(&d, &cfg);
    events_t out = {0};
    s_seed = seed;
    for (uint32_t k = 0; k < 200u; ++k) {   /* 10 s, peak at 5 s */
        const float t = (float)(k * TICK_MS) / 1000.0f;
        feed(&d, ch, pass_db(t, 5.0f, peak_db, 10.0f, 20.0f) + gauss(sigma), t_start_ms + k * TICK_MS, &out);
    }
    return out;
}

static int near_ms(uint32_t a, uint32_t b, uint32_t tol) { return (int32_t)(a - b) <= (int32_t)tol && (int32_t)(b - a) <= (int32_t)tol; }

int main(void)
{
    /* Configuration */
    pd_config_t cfg = pd_config_default();
    check("defaults: valid, drop 3 dB, refractory 1.5 s", pd_config_valid(&cfg) && cfg.drop_db == 3.0f &&
          cfg.refractory_ms == 1500u);
    pd_config_t bad = cfg; bad.min_peak_db = cfg.active_db - 1.0f;
    check("min peak under the activity threshold is refused", !pd_config_valid(&bad));
    bad = cfg; bad.alpha = 0.0f;
    check("alpha 0 is refused", !pd_config_valid(&bad));
    pass_detector_t d;
    pd_init(&d, &cfg);
    check("pd_set_config refuses an invalid set and keeps the old one", !pd_set_config(&d, &bad) && d.cfg.alpha == cfg.alpha);
    check("state names", !strcmp(pd_state_name(PD_RISING), "RISING") && !strcmp(pd_state_name(PD_COOLDOWN), "COOLDOWN"));

    /* Single clean pass */
    events_t e = run_pass(35.0f, 0.0f, 1u, 0u, 0u);
    check("single pass: exactly one event", e.n == 1u);
    check("single pass: on its channel, peak ~35 dB at ~5 s", e.n == 1u && e.ev[0].channel == 0u &&
          fabsf(e.ev[0].peak_db - 35.0f) < 1.0f && near_ms(e.ev[0].peak_ms, 5000u, 200u));
    check("single pass: rings after the peak, within 1 s", e.n == 1u && e.ev[0].at_ms > e.ev[0].peak_ms &&
          e.ev[0].at_ms - 5000u < 1000u);

    /* Noisy pass: 2 dB per sample, 50 seeds, every one rings exactly once near the peak */
    unsigned once = 0, timely = 0;
    for (uint32_t seed = 1; seed <= 50u; ++seed) {
        e = run_pass(35.0f, 2.0f, seed, 3u, 0u);
        once += e.n == 1u;
        timely += e.n == 1u && e.ev[0].channel == 3u && e.ev[0].at_ms >= 4500u && e.ev[0].at_ms <= 6500u;
    }
    printf("     noisy pass: %u/50 exactly once, %u/50 within -0.5..+1.5 s\n", once, timely);
    check("noisy pass (sigma 2 dB): one event per pass, near the peak, 50/50 seeds", once == 50u && timely == 50u);

    /* Timestamps wrapping through 2^32 ms */
    e = run_pass(35.0f, 1.0f, 7u, 1u, 0xFFFFF000u);
    check("pass across the uint32 millisecond wrap: one event", e.n == 1u && near_ms(e.ev[0].peak_ms, 0xFFFFF000u + 5000u, 300u));

    /* Far drone: maximum under min_peak_db */
    e = run_pass(22.0f, 1.0f, 3u, 0u, 0u);
    check("far drone (peak 22 dB < 25): never rings", e.n == 0u);

    /* Two drones, two channels, same time (interleaved samples) */
    pd_init(&d, &cfg);
    e = (events_t){0};
    s_seed = 11u;
    for (uint32_t k = 0; k < 200u; ++k) {
        const float t = (float)(k * TICK_MS) / 1000.0f;
        feed(&d, 2u, pass_db(t, 4.0f, 34.0f, 10.0f, 20.0f) + gauss(1.5f), k * TICK_MS, &e);
        feed(&d, 5u, pass_db(t, 4.3f, 38.0f, 8.0f, 25.0f) + gauss(1.5f), k * TICK_MS, &e);
    }
    unsigned on2 = 0, on5 = 0;
    for (unsigned k = 0; k < e.n; ++k) {
        on2 += e.ev[k].channel == 2u && near_ms(e.ev[k].peak_ms, 4000u, 400u);
        on5 += e.ev[k].channel == 5u && near_ms(e.ev[k].peak_ms, 4300u, 400u);
    }
    check("two drones on R3 and R6 together: one event each, own peak time", e.n == 2u && on2 == 1u && on5 == 1u);

    /* Hovering drone: approaches, then stays 30 s with noise and a slow wobble */
    pd_init(&d, &cfg);
    e = (events_t){0};
    s_seed = 5u;
    for (uint32_t k = 0; k < 700u; ++k) {   /* 35 s */
        const float t = (float)(k * TICK_MS) / 1000.0f;
        const float base = t < 5.0f ? pass_db(t, 5.0f, 30.0f, 10.0f, 10.0f) : 30.0f + 2.0f * sinf(t * 1.3f);
        feed(&d, 4u, base + gauss(2.0f), k * TICK_MS, &e);
    }
    check("hovering drone (30 dB +- wobble and noise, 30 s): rings only once", e.n == 1u);

    /* Noise only, under the activity threshold */
    pd_init(&d, &cfg);
    e = (events_t){0};
    s_seed = 9u;
    bool rising = false;
    for (uint32_t k = 0; k < 2400u; ++k) {  /* 2 minutes */
        feed(&d, 0u, 10.0f + gauss(2.5f), k * TICK_MS, &e);
        rising |= d.ch[0].state != PD_IDLE;
    }
    check("noise under the threshold (10 dB, sigma 2.5, 2 min): never rings", e.n == 0u);
    check("noise under the threshold: never leaves IDLE", !rising);

    /* Two laps 20 s apart: re-armed between them */
    pd_init(&d, &cfg);
    e = (events_t){0};
    s_seed = 13u;
    for (uint32_t k = 0; k < 600u; ++k) {
        const float t = (float)(k * TICK_MS) / 1000.0f;
        const float a = pass_db(t, 5.0f, 35.0f, 10.0f, 20.0f), b = pass_db(t, 25.0f, 33.0f, 10.0f, 20.0f);
        feed(&d, 6u, (a > b ? a : b) + gauss(1.0f), k * TICK_MS, &e);
    }
    check("two laps 20 s apart: two events", e.n == 2u && near_ms(e.ev[0].peak_ms, 5000u, 300u) &&
          near_ms(e.ev[1].peak_ms, 25000u, 300u));

    /* Refractory: the signal dips under release and returns within 1.5 s */
    pd_init(&d, &cfg);
    e = (events_t){0};
    for (uint32_t k = 0; k < 200u; ++k) {
        const float t = (float)(k * TICK_MS) / 1000.0f;
        float db = pass_db(t, 5.0f, 35.0f, 10.0f, 20.0f);
        if (t >= 5.5f && t < 5.9f) db = FLOOR_DB;                          /* behind an obstacle */
        if (t >= 5.9f && t < 6.4f) db = 34.0f - 20.0f * fabsf(t - 6.1f);  /* a second short peak */
        if (t >= 6.4f) db = FLOOR_DB;
        feed(&d, 1u, db, k * TICK_MS, &e);
    }
    check("second peak inside the refractory period: still one event", e.n == 1u);

    /* Stale channel: a rising pass whose samples stop */
    pd_init(&d, &cfg);
    e = (events_t){0};
    for (uint32_t k = 0; k < 90u; ++k)
        feed(&d, 0u, pass_db((float)(k * TICK_MS) / 1000.0f, 5.0f, 35.0f, 10.0f, 20.0f), k * TICK_MS, &e);
    const bool was_rising = d.ch[0].state == PD_RISING;
    pd_expire(&d, 89u * TICK_MS + 1500u);
    check("samples stop while RISING: pd_expire returns to IDLE, no event", was_rising && d.ch[0].state == PD_IDLE &&
          e.n == 0u);
    feed(&d, 0u, 12.0f, 89u * TICK_MS + 1600u, &e);
    check("after a gap the smoothing restarts from the new sample", d.ch[0].smooth == 12.0f && e.n == 0u);
    feed(&d, 0u, NAN, 89u * TICK_MS + 1650u, &e);
    check("a NaN sample is ignored", d.ch[0].smooth == 12.0f && d.ch[0].last_ms == 89u * TICK_MS + 1600u);
    check("out-of-range channel is ignored", !pd_update(&d, PD_CHANNELS, 40.0f, 0u, NULL));

    printf("%u/%u pass detector checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
