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

    /* History: newest first, splits into the lap, a missed gate reads 0. */
    {
        race_timing_t rt;
        rt_init(&rt, &(rt_config_t){ .num_gates = 3 });
        rt_cross(&rt, 0, 2, 1000, NULL);
        const rt_channel_t *c = rt_channel(&rt, 2);
        CHECK(rt_recent_lap(c, 0) == NULL, "no lap kept before the first one ends");
        rt_cross(&rt, 1, 2, 2000, NULL);
        rt_cross(&rt, 2, 2, 3500, NULL);
        rt_cross(&rt, 0, 2, 5000, NULL);        /* lap 1: 4000 ms, splits 1000 and 2500 */
        rt_cross(&rt, 2, 2, 7000, NULL);        /* lap 2 misses gate 1 */
        rt_cross(&rt, 0, 2, 8000, NULL);
        const rt_lap_t *l = rt_recent_lap(c, 0);
        CHECK(l && l->number == 2 && l->lap_ms == 3000, "age 0 is the last lap");
        CHECK(l && l->split_ms[1] == 0 && l->split_ms[2] == 2000, "a missed gate reads 0");
        l = rt_recent_lap(c, 1);
        CHECK(l && l->number == 1 && l->lap_ms == 4000, "age 1 is the lap before");
        CHECK(l && l->split_ms[1] == 1000 && l->split_ms[2] == 2500, "splits count from the lap's start");
        CHECK(rt_recent_lap(c, 2) == NULL, "nothing past the laps run");

        for (unsigned i = 1; i <= 2u * RT_HISTORY; i++) rt_cross(&rt, 0, 2, 8000 + i * 100, NULL);
        const unsigned total = 2 + 2u * RT_HISTORY;
        l = rt_recent_lap(c, 0);
        CHECK(l && l->number == total, "the ring keeps the newest lap");
        l = rt_recent_lap(c, RT_HISTORY - 1);
        CHECK(l && l->number == total - RT_HISTORY + 1, "and the 10th newest");
        CHECK(rt_recent_lap(c, RT_HISTORY) == NULL, "not the one before");
    }

    /* Reset of one channel. */
    {
        race_timing_t rt;
        rt_init(&rt, &(rt_config_t){ .num_gates = 2 });
        for (uint32_t t = 0; t <= 2000; t += 1000) { rt_cross(&rt, 0, 1, t, NULL); rt_cross(&rt, 0, 3, t + 10, NULL); }
        rt_cross(&rt, 1, 1, 2500, NULL);
        CHECK(rt_reset_channel(&rt, 1), "a tracked channel resets");
        const rt_channel_t *c = rt_channel(&rt, 1);
        CHECK(c && c->laps == 0 && c->last_lap_ms == 0 && c->best_lap_ms == 0 && c->last_gate == -1, "its times are gone");
        CHECK(c && rt_recent_lap(c, 0) == NULL, "and its history");
        CHECK(c && c->last_cross_ms == 2500, "when it was last seen stays");
        CHECK(rt_channel(&rt, 3)->laps == 2, "the other channel keeps its laps");
        CHECK(!rt_cross(&rt, 0, 1, 3000, NULL), "the lap in progress is dropped: this one only opens a lap");
        uint32_t lap = 0;
        CHECK(rt_cross(&rt, 0, 1, 3900, &lap) && lap == 900 && rt_channel(&rt, 1)->laps == 1, "the count starts again");
        CHECK(rt_recent_lap(rt_channel(&rt, 1), 0)->number == 1, "from lap 1");
        CHECK(!rt_reset_channel(&rt, 7), "an unknown channel is refused");
    }

    /* A board's clock onto the fleet clock. */
    {
        rt_clock_t clk = { 0 };
        CHECK(rt_clock_to_fleet(&clk, 1000, 51050) == 51050, "first timestamp: offset as it came");
        CHECK(rt_clock_to_fleet(&clk, 2000, 52200) == 52050, "arriving later does not move the offset");
        CHECK(rt_clock_to_fleet(&clk, 3000, 53010) == 53010, "arriving sooner tightens it");
        const uint32_t a = rt_clock_to_fleet(&clk, 4000, 54150), b = rt_clock_to_fleet(&clk, 11300, 61400);
        CHECK(b - a == 7300, "the time between two timestamps survives any lateness");
        CHECK(rt_clock_to_fleet(&clk, 500, 70520) == 70520, "a reboot of the board is learnt again");
        rt_clock_t early = { 0 };          /* the board up long before the fleet clock */
        CHECK(rt_clock_to_fleet(&early, 100000, 5000) == 5000, "a board ahead of the fleet clock");
        CHECK(rt_clock_to_fleet(&early, 101000, 6100) == 6000, "still ordered right");
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
