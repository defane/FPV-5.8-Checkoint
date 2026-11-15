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

/* ---- Relative re-arm (pd_set_rearm). Traces at 20 Hz fed with weight 1 (each sample is the
 * smoothed level), pd_config_default: active 15 dB, release 12 dB, drop 3 dB, min peak 25 dB,
 * refractory 1.5 s. A gap in a trace (NaN) is no sample. */
static float tri(float t, float tc, float peak, float half_s) { return peak * fmaxf(0.0f, 1.0f - fabsf(t - tc) / half_s); }

/* A pass from quiet at 3 s (50 dB), then the drone stays 30 dB up: passes back to 50 at 13 and 23 s. */
static float plateau_laps(float t)
{
    if (t < 2.0f) return 0.0f;
    const float pass = fmaxf(fmaxf(50.0f - 20.0f * fabsf(t - 3.0f), 50.0f - 20.0f * fabsf(t - 13.0f)), 50.0f - 20.0f * fabsf(t - 23.0f));
    return t < 3.0f ? pass : fmaxf(30.0f, pass);
}
/* A pass at 3 s, then a slow fall from 50 to 25 dB over 10 s with a 4 dB peak-to-peak wobble (more
 * than drop_db), then bumps on the 25 dB: +10 dB at 20 s (under the drop), +20 dB at 30 s. */
static float wobble_fall(float t)
{
    if (t < 2.0f) return 0.0f;
    if (t < 4.0f) return 50.0f - 20.0f * fabsf(t - 3.0f);
    const float base = t < 14.0f ? 50.0f - 2.5f * (t - 4.0f) : 25.0f;
    return base + 2.0f * sinf(6.2831853f * t / 0.8f) + tri(t, 20.0f, 10.0f, 1.0f) + tri(t, 30.0f, 20.0f, 1.0f);
}
/* A slow pass held 45 dB up from 3 to 12 s, with two deep fades (25 dB for 150 ms at 6 s, for 300 ms
 * at 9 s), then gone. */
static float faded_hold(float t)
{
    if (t < 2.0f) return 0.0f;
    if (t < 3.0f) return 50.0f - 20.0f * (3.0f - t);
    if (t > 12.0f) return 0.0f;
    if ((t >= 6.0f && t < 6.15f) || (t >= 9.0f && t < 9.3f)) return 25.0f;
    return t < 3.25f ? 50.0f - 20.0f * (t - 3.0f) : 45.0f;
}
/* The same hold, but the drone leaves for 3 s (25 dB, 6-9 s) and comes back past the gate at 9.5 s. */
static float left_and_back(float t)
{
    if (t < 6.0f || t >= 12.0f) return faded_hold(t);
    if (t < 9.0f) return 25.0f;
    return fmaxf(45.0f, 50.0f - 20.0f * fabsf(t - 9.5f));
}
/* The plateau with no sample from 4 to 6 s (longer than stale_ms). */
static float plateau_gap(float t) { return t >= 4.0f && t < 6.0f ? NAN : plateau_laps(t); }

/* FNV-1a over an event's fields (not its padding). */
static uint32_t hash_event(uint32_t h, const pd_event_t *e)
{
    uint32_t bits;
    memcpy(&bits, &e->peak_db, sizeof bits);
    const uint32_t f[5] = {e->channel, bits, e->peak_ms, e->at_ms, e->rel};
    for (unsigned i = 0; i < 5u; i++)
        for (unsigned k = 0; k < 4u; k++) h = (h ^ ((f[i] >> (8u * k)) & 0xffu)) * 16777619u;
    return h;
}

static events_t run_trace(float (*f)(float), float seconds, float rearm_db, float sigma, uint32_t seed, pass_detector_t *out)
{
    static pass_detector_t d;
    pd_config_t cfg = pd_config_default();
    pd_init(&d, &cfg);
    pd_set_rearm(&d, rearm_db);
    events_t e = {0};
    s_seed = seed;
    for (uint32_t k = 0; k * TICK_MS <= (uint32_t)(seconds * 1000.0f); ++k) {
        const float db = f((float)(k * TICK_MS) / 1000.0f) + gauss(sigma);
        pd_event_t ev;
        if (pd_update_w(&d, 0u, db, 1.0f, k * TICK_MS, &ev) && e.n < 8u) e.ev[e.n++] = ev;
    }
    if (out) *out = d;
    return e;
}



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
    /* The only guard on a pushed set (the C3 web copies any value): stale_ms 0 would make every
     * sample stale and the gate silent. */
    bad = cfg; bad.stale_ms = 0u;
    check("stale_ms 0 is refused", !pd_config_valid(&bad));
    bad = cfg; bad.drop_db = 0.0f;
    check("drop 0 is refused", !pd_config_valid(&bad));
    bad = cfg; bad.hysteresis_db = 0.0f;
    check("hysteresis 0 is refused", !pd_config_valid(&bad));
    bad = cfg; bad.min_peak_db = cfg.active_db;
    check("min peak equal to the activity threshold is accepted", pd_config_valid(&bad));
    bad = cfg; bad.alpha = 0.0f;   /* the invalid set pd_set_config is offered */
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
    pd_init(&d, &cfg);
    e = (events_t){0};
    s_seed = 3u;
    bool far_rose = false;
    for (uint32_t k = 0; k < 200u; ++k) {
        feed(&d, 0u, pass_db((float)(k * TICK_MS) / 1000.0f, 5.0f, 22.0f, 10.0f, 20.0f) + gauss(1.0f), k * TICK_MS, &e);
        far_rose |= d.ch[0].state == PD_RISING;
    }
    check("far drone: RISING, then released to IDLE once it fades", far_rose && d.ch[0].state == PD_IDLE && e.n == 0u);

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

    /* pd_update_w: weight 1 is no smoothing, exactly; pd_update is pd_update_w(alpha). */
    pass_detector_t w, u, v;
    pd_init(&w, &cfg);
    pd_init(&u, &cfg);
    pd_init(&v, &cfg);
    s_seed = 17u;
    bool exact = true, same = true;
    unsigned events_w = 0, events_u = 0;
    for (uint32_t k = 0; k < 20000u; ++k) {
        const uint32_t now = k * 2u + (k / 3000u) * 1500u;   /* 2 ms, and a stale gap now and then */
        const unsigned ch = k % 3u;
        const float db = 20.0f * uniform() + (k % 400u < 60u ? 25.0f : 0.0f) + gauss(1.0f);
        pd_event_t eu, ev;
        events_w += pd_update_w(&w, ch, db, 1.0f, now, NULL);
        exact &= w.ch[ch].smooth == db;
        const bool ru = pd_update(&u, ch, db, now, &eu), rv = pd_update_w(&v, ch, db, cfg.alpha, now, &ev);
        events_u += ru;
        same &= ru == rv && (!ru || (eu.channel == ev.channel && eu.peak_db == ev.peak_db && eu.peak_ms == ev.peak_ms &&
                                     eu.at_ms == ev.at_ms));
        same &= u.ch[ch].state == v.ch[ch].state && u.ch[ch].smooth == v.ch[ch].smooth &&
                u.ch[ch].peak == v.ch[ch].peak && u.ch[ch].peak_ms == v.ch[ch].peak_ms &&
                u.ch[ch].cooldown_until_ms == v.ch[ch].cooldown_until_ms;
    }
    printf("     random sequence: %u events at weight 1, %u at alpha\n", events_w, events_u);
    check("pd_update_w weight 1: smooth == level exactly, every sample", exact);
    check("pd_update equals pd_update_w(alpha), every sample", same && events_u > 0u);

    /* Relative re-arm. */
    pass_detector_t r;
    pd_init(&r, &cfg);
    check("re-arm: off after pd_init", r.rearm_db == 0.0f);
    pd_set_rearm(&r, 16.0f);
    const float on = r.rearm_db;
    pd_set_rearm(&r, -3.0f);
    const float neg = r.rearm_db;
    pd_set_rearm(&r, NAN);
    check("re-arm: pd_set_rearm takes 16, turns off on a negative or NaN drop", on == 16.0f && neg == 0.0f && r.rearm_db == 0.0f);

    /* A drone that stays 30 dB up between laps (release 12 dB): today's re-arm rings its first pass only. */
    e = run_trace(plateau_laps, 30.0f, 0.0f, 0.5f, 3u, NULL);
    check("re-arm 0: a drone staying 30 dB up rings its first pass only, as before", e.n == 1u && !e.ev[0].rel);
    e = run_trace(plateau_laps, 30.0f, 16.0f, 0.5f, 3u, NULL);
    check("re-arm 16: every lap of a drone staying 30 dB up rings", e.n == 3u && near_ms(e.ev[0].peak_ms, 3000u, 100u) &&
          near_ms(e.ev[1].peak_ms, 13000u, 100u) && near_ms(e.ev[2].peak_ms, 23000u, 100u));
    check("re-arm 16: the first by the release, the next by the relative re-arm", e.n == 3u && !e.ev[0].rel && e.ev[1].rel && e.ev[2].rel);
    (void)run_trace(plateau_laps, 4.5f, 16.0f, 0.5f, 3u, &r);
    const bool cooling = r.ch[0].state == PD_COOLDOWN && r.ch[0].peak > 49.0f;
    (void)run_trace(plateau_laps, 8.0f, 16.0f, 0.5f, 3u, &r);
    check("re-arm 16: COOLDOWN until the 1 s average is 16 dB under the peak, then IDLE (relative)",
          cooling && r.ch[0].state == PD_IDLE && r.ch[0].rel && r.ch[0].trough < 32.0f && r.ch[0].trough > 28.0f);

    /* The pitfall: right after a relative re-arm the level is still far over active_db, so the rest of
     * the fall and its wobble (4 dB, more than drop_db) would ring at once without the trough. */
    e = run_trace(wobble_fall, 34.0f, 16.0f, 0.3f, 4u, NULL);
    check("re-arm 16: the fall and a 4 dB wobble after the re-arm ring nothing, nor a 10 dB bump; a 20 dB one does",
          e.n == 2u && near_ms(e.ev[0].peak_ms, 3000u, 100u) && near_ms(e.ev[1].peak_ms, 30000u, 150u) && e.ev[1].rel);
    e = run_trace(wobble_fall, 34.0f, 0.0f, 0.3f, 4u, NULL);
    check("re-arm 0: the same trace rings its first pass only", e.n == 1u);

    /* Deep fades inside one slow pass: 20 dB for 150 and 300 ms, 3 and 6 s after the peak. The 1 s
     * average barely moves, so no re-arm and no double count; leaving for 3 s does re-arm. */
    e = run_trace(faded_hold, 14.0f, 16.0f, 0.3f, 5u, NULL);
    check("re-arm 16: two 20 dB fades inside a held pass: one pass", e.n == 1u);
    (void)run_trace(faded_hold, 9.4f, 16.0f, 0.3f, 5u, &r);
    check("re-arm 16: the fades never re-arm it (COOLDOWN after the second)", r.ch[0].state == PD_COOLDOWN && !r.ch[0].rel);
    e = run_trace(left_and_back, 14.0f, 16.0f, 0.3f, 5u, NULL);
    check("re-arm 16: leaving for 3 s and coming back is a second pass", e.n == 2u && near_ms(e.ev[1].peak_ms, 9500u, 100u) && e.ev[1].rel);

    /* Laps that fall under the release between passes: the classic re-arm, the same passes to the bit. */
    {
        unsigned same = 0, laps_ok = 0;
        for (uint32_t k = 0; k < 20u; ++k) {
            float peaks[12], troughs[12];
            s_seed = 900u + k;
            for (unsigned i = 0; i < 12u; i++) { peaks[i] = 30.0f + 20.0f * uniform(); troughs[i] = 5.0f * uniform(); }
            pass_detector_t a, b;
            pd_init(&a, &cfg);
            pd_init(&b, &cfg);
            pd_set_rearm(&b, 16.0f);
            unsigned na = 0, nb = 0;
            uint32_t ha = 2166136261u, hb = 2166136261u;   /* FNV-1a over every event's fields */
            s_seed = 77u + k;
            for (uint32_t t = 0; t < 120000u; t += TICK_MS) {
                const unsigned lap = t / 10000u;
                const float x = (float)(t % 10000u) / 1000.0f - 5.0f;   /* the gate 5 s into each lap */
                const float db = troughs[lap] + (peaks[lap] - troughs[lap]) * expf(-0.5f * x * x / 0.36f) + gauss(1.0f);
                pd_event_t ev;
                if (pd_update(&a, 0u, db, t, &ev)) { na++; ha = hash_event(ha, &ev); }
                if (pd_update(&b, 0u, db, t, &ev)) { nb++; hb = hash_event(hb, &ev); }
            }
            same += na == nb && ha == hb;
            laps_ok += na == 12u;
        }
        check("laps that fall under the release: re-arm 16 rings the same passes as 0, to the bit (20 runs of 12 laps)",
              same == 20u && laps_ok == 20u);
    }

    /* A gap over stale_ms in COOLDOWN: the peak is forgotten, the reference restarts from the level
     * the channel comes back at. */
    (void)run_trace(plateau_gap, 6.2f, 16.0f, 0.0f, 6u, &r);
    check("re-arm 16: after a stale gap in COOLDOWN, the reference restarts from the level it comes back at",
          r.ch[0].state == PD_COOLDOWN && !r.ch[0].rel && r.ch[0].peak > 29.0f && r.ch[0].peak < 31.0f);

    /* pd_resume (the pair guard's long hold): the channel starts again at the level it comes back
     * at, whatever it was before. A pass rang at 3 s and the drone stays 30 dB up; blind from 4 to
     * 6 s, it comes back at 30: relatively armed there, a 4 dB wobble rings nothing, a +20 dB pass
     * does. Under the release it is armed; with the re-arm off it waits in COOLDOWN as before. */
    {
        (void)run_trace(plateau_laps, 4.0f, 16.0f, 0.5f, 3u, &r);
        const bool was_cooling = r.ch[0].state == PD_COOLDOWN && r.ch[0].peak > 49.0f;
        pd_resume(&r, 0u, 30.0f, 6000u);
        const pd_channel_t c0 = r.ch[0];
        unsigned n = 0, rel = 0;
        for (uint32_t t = 6000u; t <= 12000u; t += TICK_MS) {
            const float x = (float)t / 1000.0f;
            const float db = 30.0f + 2.0f * sinf(6.2831853f * x / 0.8f) + tri(x, 10.0f, 20.0f, 0.6f);
            pd_event_t ev;
            if (pd_update_w(&r, 0u, db, 1.0f, t, &ev)) { n++; rel += ev.rel && near_ms(ev.peak_ms, 10000u, 100u); }
        }
        check("pd_resume over the release: relatively armed, trough and smoothing at the level it comes back at",
              was_cooling && c0.state == PD_IDLE && c0.rel && c0.primed && c0.trough == 30.0f && c0.smooth == 30.0f &&
                  c0.slow == 30.0f && c0.last_ms == 6000u);
        check("pd_resume: a 4 dB wobble at that level rings nothing, the next +20 dB pass rings (rel)", n == 1u && rel == 1u);
        pd_resume(&r, 0u, 8.0f, 13000u);
        check("pd_resume under the release: armed (IDLE, not relative)", r.ch[0].state == PD_IDLE && !r.ch[0].rel);
        pd_resume(&r, 0u, 13.0f, 13000u);   /* the release is 12 dB, active_db 15 */
        check("pd_resume between the release and active_db: relatively armed there",
              r.ch[0].state == PD_IDLE && r.ch[0].rel && r.ch[0].trough == 13.0f);
        pd_resume(&r, 0u, 8.0f, 13000u);
        const pd_channel_t keep = r.ch[0];
        pd_resume(&r, 0u, NAN, 13050u);
        pd_resume(&r, PD_CHANNELS, 30.0f, 13050u);
        check("pd_resume: a NaN level or a bad channel changes nothing", !memcmp(&keep, &r.ch[0], sizeof(keep)));
        pass_detector_t off;
        pd_init(&off, &cfg);
        pd_resume(&off, 0u, 30.0f, 1000u);
        bool held = off.ch[0].state == PD_COOLDOWN && off.ch[0].cooldown_until_ms == 1000u;
        for (uint32_t t = 1000u; t <= 6000u; t += TICK_MS) held &= !pd_update_w(&off, 0u, 30.0f, 1.0f, t, NULL) && off.ch[0].state == PD_COOLDOWN;
        (void)pd_update_w(&off, 0u, 5.0f, 1.0f, 6050u, NULL);
        check("pd_resume, re-arm off: COOLDOWN until under the release, as before", held && off.ch[0].state == PD_IDLE);
    }

    printf("%u/%u pass detector checks passed\n", s_run - s_fail, s_run);
    return s_fail != 0;
}
