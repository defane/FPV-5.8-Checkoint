/* Host test for components/race_timing. Build/run:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/race_timing/include \
 *      test_race_timing.c ../components/race_timing/race_timing.c -o /tmp/t && /tmp/t
 */
#include "race_timing.h"
#include <stdio.h>

static int g_pass, g_fail;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { printf("ok   %s\n", name); g_pass++; }                    \
        else { printf("FAIL %s\n", name); g_fail++; }                         \
    } while (0)

int main(void) {
    /* 3 gates: 0 start/finish, 1 and 2 sectors. One channel, two clean laps. */
    {
        race_timing_t rt;
        rt_init(&rt, &(rt_config_t){ .num_gates = 3 });
        uint32_t lap = 0;
        CHECK(!rt_cross(&rt, 0, 40, 0, &lap), "first start/finish opens a lap, no lap time");
        rt_cross(&rt, 1, 40, 1000, NULL);
        rt_cross(&rt, 2, 40, 2000, NULL);
        bool done = rt_cross(&rt, 0, 40, 3000, &lap);
        CHECK(done && lap == 3000, "lap 1 completes at 3000 ms");
        CHECK(rt_sector_ms(&rt, 40, 1) == 0, "sectors reset at the new lap start");

        rt_cross(&rt, 1, 40, 3800, NULL);
        CHECK(rt_sector_ms(&rt, 40, 1) == 800, "sector 1 = 800 ms into lap 2");
        rt_cross(&rt, 2, 40, 4900, NULL);
        CHECK(rt_sector_ms(&rt, 40, 2) == 1100, "sector 2 = 1100 ms");
        done = rt_cross(&rt, 0, 40, 6000, &lap);
        const rt_channel_t *c = rt_channel(&rt, 40);
        CHECK(done && lap == 3000 && c->laps == 2 && c->best_lap_ms == 3000,
              "lap 2 = 3000, two laps, best 3000");
    }

    /* Best lap tracks the fastest. */
    {
        race_timing_t rt;
        rt_init(&rt, &(rt_config_t){ .num_gates = 1 });  /* lap gate only */
        uint32_t lap;
        rt_cross(&rt, 0, 1, 0, NULL);
        rt_cross(&rt, 0, 1, 5000, &lap);   /* lap 5000 */
        rt_cross(&rt, 0, 1, 9000, &lap);   /* lap 4000 (faster) */
        rt_cross(&rt, 0, 1, 15000, &lap);  /* lap 6000 */
        const rt_channel_t *c = rt_channel(&rt, 1);
        CHECK(c->laps == 3 && c->best_lap_ms == 4000 && c->last_lap_ms == 6000,
              "best lap is the fastest of three");
    }

    /* Two channels are independent. */
    {
        race_timing_t rt;
        rt_init(&rt, &(rt_config_t){ .num_gates = 2 });
        uint32_t lap;
        rt_cross(&rt, 0, 10, 0, NULL);
        rt_cross(&rt, 0, 20, 100, NULL);
        rt_cross(&rt, 1, 10, 900, NULL);
        rt_cross(&rt, 1, 20, 1200, NULL);
        bool a = rt_cross(&rt, 0, 10, 2000, &lap);
        uint32_t lapb; bool b = rt_cross(&rt, 0, 20, 2600, &lapb);
        CHECK(a && lap == 2000 && b && lapb == 2500, "two channels time independently");
        CHECK(rt_channel(&rt, 10)->laps == 1 && rt_channel(&rt, 20)->laps == 1,
              "each channel has one lap");
    }

    /* A missed middle gate: the lap still times; that sector is unavailable. */
    {
        race_timing_t rt;
        rt_init(&rt, &(rt_config_t){ .num_gates = 3 });
        uint32_t lap;
        rt_cross(&rt, 0, 5, 0, NULL);
        /* gate 1 missed */
        rt_cross(&rt, 2, 5, 2000, NULL);
        bool done = rt_cross(&rt, 0, 5, 3000, &lap);
        CHECK(done && lap == 3000, "lap times despite a missed gate");
        /* reopen lap, cross only gate 2 */
        rt_cross(&rt, 2, 5, 4500, NULL);
        CHECK(rt_sector_ms(&rt, 5, 1) == 0 && rt_sector_ms(&rt, 5, 2) == 0,
              "sector with a missing endpoint is unavailable");
    }

    /* Out-of-range gate and unknown channel on a full table are ignored. */
    {
        race_timing_t rt;
        rt_init(&rt, &(rt_config_t){ .num_gates = 2 });
        CHECK(!rt_cross(&rt, 5, 1, 0, NULL), "out-of-range gate ignored");
        for (unsigned ch = 0; ch < RT_MAX_CHANNELS; ch++) rt_cross(&rt, 0, (uint8_t)ch, 0, NULL);
        CHECK(rt_channel(&rt, 200) == NULL && !rt_cross(&rt, 0, 200, 10, NULL),
              "channel beyond the table is dropped");
        CHECK(rt.num_channels == RT_MAX_CHANNELS, "channel table is full, not overrun");
    }

    /* num_gates is clamped. */
    {
        race_timing_t rt;
        rt_init(&rt, &(rt_config_t){ .num_gates = 99 });
        CHECK(rt.cfg.num_gates == RT_MAX_GATES, "num_gates clamped to RT_MAX_GATES");
        rt_init(&rt, &(rt_config_t){ .num_gates = 0 });
        CHECK(rt.cfg.num_gates == 1, "num_gates floored to 1");
    }

    printf("%d/%d race_timing checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
