/* race_timing: lap/sector timing from gate-crossing events. Pure C, no float,
 * no hardware. The C6 feeds it (gate, channel, fleet_ms) crossings; it keeps,
 * per channel (= pilot), the current lap's gate times, lap count, last and best
 * lap, per-sector times and its last RT_HISTORY laps.
 *
 * Gates are ordered 0..num_gates-1 along the track; gate 0 is start/finish and
 * bounds a lap. A crossing of gate 0 closes the lap in progress (if any) and
 * opens the next. Sector k (1..num_gates-1) is the time from gate k-1 to gate k
 * within the current lap. Times are uint32 milliseconds in fleet time. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_MAX_GATES    16u
#define RT_MAX_CHANNELS 8u   /* pilots tracked concurrently */
#define RT_HISTORY      10u  /* completed laps kept per channel */

typedef struct {
    uint8_t num_gates;   /* 1..RT_MAX_GATES; gate 0 is start/finish */
} rt_config_t;

typedef struct {
    uint32_t number;                   /* 1 for the channel's first lap */
    uint32_t lap_ms;
    uint32_t split_ms[RT_MAX_GATES];   /* time into the lap at gate k, 0 if missed; [0] unused */
} rt_lap_t;

typedef struct {
    uint8_t  channel;
    bool     active;
    uint32_t laps;                 /* completed laps */
    uint32_t last_lap_ms;          /* last completed lap, 0 if none */
    uint32_t best_lap_ms;          /* best completed lap, 0 if none */
    int16_t  last_gate;            /* last gate crossed, -1 if none */
    uint32_t last_cross_ms;
    bool     gate_seen[RT_MAX_GATES];   /* crossed this lap (index 0 = lap start) */
    uint32_t gate_ms[RT_MAX_GATES];     /* crossing time this lap */
    rt_lap_t recent[RT_HISTORY];        /* ring, read with rt_recent_lap() */
} rt_channel_t;

typedef struct {
    rt_config_t  cfg;
    rt_channel_t ch[RT_MAX_CHANNELS];
    uint8_t      num_channels;     /* channels discovered so far */
} race_timing_t;

/* num_gates is clamped to [1, RT_MAX_GATES]. */
void rt_init(race_timing_t *rt, const rt_config_t *cfg);
/* Clear all per-channel progress for a new race; keeps the gate count. */
void rt_reset(race_timing_t *rt);

/* Clear one channel's times and lap in progress; it stays tracked, and last_cross_ms
 * still tells when it was last seen. false if the channel is not tracked. */
bool rt_reset_channel(race_timing_t *rt, uint8_t channel);

/* Ingest one crossing. Returns true when it completes a lap; *lap_ms_out (may
 * be NULL) then holds the lap time. Out-of-range gates and, once all channel
 * slots are used, unknown channels are ignored. */
bool rt_cross(race_timing_t *rt, uint8_t gate, uint8_t channel,
              uint32_t fleet_ms, uint32_t *lap_ms_out);

/* Sector k (1..num_gates-1) of the current lap for channel, or 0 if either end
 * has not been crossed this lap. */
uint32_t rt_sector_ms(const race_timing_t *rt, uint8_t channel, uint8_t gate);

/* Per-channel state, or NULL if the channel is not tracked. */
const rt_channel_t *rt_channel(const race_timing_t *rt, uint8_t channel);

/* The completed lap `age` laps back (0 = the last one); NULL past the laps run or kept. */
const rt_lap_t *rt_recent_lap(const rt_channel_t *c, unsigned age);

/* Puts a board's own timestamps on the fleet clock, for a board that has no beacons.
 * Start it zeroed. Returns the fleet time of local_ms; fleet_now_ms is when it arrived. */
typedef struct {
    bool     valid;
    uint32_t offset_ms, last_ms;
} rt_clock_t;
uint32_t rt_clock_to_fleet(rt_clock_t *clk, uint32_t local_ms, uint32_t fleet_now_ms);

#ifdef __cplusplus
}
#endif
