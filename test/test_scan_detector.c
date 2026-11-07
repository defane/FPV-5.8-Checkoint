/* Host test for components/pass_detector's scan detector. Build/run:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/pass_detector/include \
 *      test_scan_detector.c ../components/pass_detector/scan_detector.c \
 *      ../components/pass_detector/pass_detector.c -lm -o /tmp/t && /tmp/t
 */
#include "scan_detector.h"
#include <math.h>
#include <stdio.h>

static int g_pass, g_fail;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { printf("ok   %s\n", name); g_pass++; }                    \
        else { printf("FAIL %s\n", name); g_fail++; }                         \
    } while (0)

#define SWEEP_MS 18u
#define QUIET_DB (-7.0f)

static uint16_t power_of(float db) { return (uint16_t)lroundf(256.0f * powf(10.0f, db / 10.0f)); }

typedef struct {
    scan_detector_t sd;
    uint32_t now;
    unsigned passes[PD_CHANNELS], total;
    pd_event_t last;
} run_t;

static void start(run_t *r)
{
    *r = (run_t){0};
    const pd_config_t c = sd_config_default();
    sd_init(&r->sd, &c);
}

/* Sweeps at these levels; a little noise so nothing sits exactly on its rest. */
static void sweeps(run_t *r, unsigned n, const float db[PD_CHANNELS])
{
    static unsigned tick;
    for (unsigned i = 0; i < n; i++) {
        uint16_t p[PD_CHANNELS];
        for (unsigned ch = 0; ch < PD_CHANNELS; ch++) p[ch] = power_of(db[ch] + ((tick++ * 7u) % 5u - 2.0f) * 0.2f);
        pd_event_t e[PD_CHANNELS];
        const unsigned got = sd_sweep(&r->sd, p, r->now, e);
        for (unsigned k = 0; k < got; k++) { r->passes[e[k].channel]++; r->total++; r->last = e[k]; }
        r->now += SWEEP_MS;
    }
}

static void quiet(run_t *r, unsigned n)
{
    const float db[PD_CHANNELS] = {QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB};
    sweeps(r, n, db);
}

static void on(run_t *r, unsigned n, unsigned ch, float db)
{
    float v[PD_CHANNELS] = {QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB};
    v[ch] = db;
    sweeps(r, n, v);
}

int main(void)
{
    run_t r;

    CHECK(sd_power_db(256) == 0.0f, "power 256 is 0 dB");
    CHECK(sd_power_db(0) == SD_FLOOR_DB, "no power reads the floor");
    CHECK(pd_config_valid(&(pd_config_t){.alpha = sd_config_default().alpha, .active_db = sd_config_default().active_db,
                                         .hysteresis_db = sd_config_default().hysteresis_db,
                                         .drop_db = sd_config_default().drop_db,
                                         .min_peak_db = sd_config_default().min_peak_db,
                                         .refractory_ms = sd_config_default().refractory_ms,
                                         .stale_ms = sd_config_default().stale_ms}), "defaults are valid");

    /* One pass, as the board shows them: +15 dB for half a second. */
    start(&r);
    quiet(&r, 400);
    CHECK(r.total == 0, "noise alone rings nothing");
    on(&r, 14, 3, 8.0f);
    const uint32_t centre = r.now;
    on(&r, 14, 3, 8.0f);
    quiet(&r, 100);
    CHECK(r.total == 1 && r.passes[3] == 1, "a pass rings once, on its channel");
    CHECK(r.last.peak_db > 10.0f && r.last.peak_db < 16.5f, "with its rise");
    CHECK(r.last.peak_ms + 300u > centre && r.last.peak_ms < centre + 300u, "and its time");
    CHECK(sd_rise_db(&r.sd, 3) < 3.0f, "the channel falls back");

    /* Two pilots side by side both count. */
    start(&r);
    quiet(&r, 400);
    {
        float v[PD_CHANNELS] = {QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB};
        v[1] = 9.0f;
        v[6] = 7.0f;
        sweeps(&r, 30, v);
    }
    quiet(&r, 100);
    CHECK(r.total == 2 && r.passes[1] == 1 && r.passes[6] == 1, "two channels at once are two passes");

    /* Nothing while the rests settle. */
    start(&r);
    on(&r, 30, 2, 12.0f);
    quiet(&r, SD_LEARN_SWEEPS);
    CHECK(r.total == 0, "nothing rings while it learns");

    /* The 4-bit IQ flickering under SD_SIGNAL_DB is no signal, however it rises. */
    start(&r);
    {
        float v[PD_CHANNELS] = {QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, QUIET_DB, -25.0f, QUIET_DB, QUIET_DB};
        sweeps(&r, 400, v);
        v[5] = -10.0f;
        sweeps(&r, 30, v);
        v[5] = -25.0f;
        sweeps(&r, 100, v);
    }
    CHECK(r.total == 0, "a rise under the signal floor rings nothing");

    /* A VTX left on keeps showing, and a drone waiting by the gate is not learnt. */
    start(&r);
    quiet(&r, 400);
    on(&r, 17000, 4, 15.0f);                 /* five minutes */
    CHECK(r.total == 0, "a steady VTX is no pass");
    CHECK(sd_rise_db(&r.sd, 4) > 15.0f, "it still shows after minutes");
    CHECK(sd_strongest(&r.sd) == 4, "as the strongest channel");
    CHECK(r.sd.rest_db[4] < QUIET_DB + SD_REST_SPREAD_DB + 1.0f, "its rest stays near the others'");
    quiet(&r, 200);                          /* it leaves */
    on(&r, 30, 4, 15.0f);                    /* first lap */
    quiet(&r, 200);
    CHECK(r.passes[4] == 2, "its first lap counts");

    /* Quick bumps of one pass stay one pass. */
    start(&r);
    quiet(&r, 400);
    for (unsigned k = 0; k < 4u; k++) {
        on(&r, 10, 0, 10.0f);
        on(&r, 8, 0, 2.0f);
    }
    quiet(&r, 100);
    CHECK(r.passes[0] == 1, "the refractory period holds a bumpy pass");

    /* A weak bump never reaches the minimum peak. */
    start(&r);
    quiet(&r, 400);
    on(&r, 30, 2, 2.0f);                     /* +9 dB */
    quiet(&r, 100);
    CHECK(r.total == 0, "under the minimum peak nothing rings");

    /* A missed reading is no sample. */
    start(&r);
    quiet(&r, 400);
    {
        uint16_t p[PD_CHANNELS];
        for (unsigned ch = 0; ch < PD_CHANNELS; ch++) p[ch] = SD_NO_DATA;
        pd_event_t e[PD_CHANNELS];
        const float rest = r.sd.rest_db[1];
        CHECK(sd_sweep(&r.sd, p, r.now, e) == 0 && r.sd.rest_db[1] == rest, "a missed reading changes nothing");
    }

    printf("%d/%d scan_detector checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
