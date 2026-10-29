/* Checkpoint pass detector: "a drone has just passed" from a per-channel
 * signal level in dB (C/N or dBm, any dB scale). Pure C, no hardware.
 *
 * Per channel: exponential smoothing, then
 *   IDLE        -> RISING       smoothed level above active_db
 *   RISING      -> PEAK_PASSED  level drop_db under the maximum reached, and
 *                               that maximum at least min_peak_db (event)
 *   RISING      -> IDLE         back under active_db - hysteresis_db first
 *   PEAK_PASSED -> COOLDOWN     on the next sample
 *   COOLDOWN    -> IDLE         refractory_ms elapsed AND the level back under
 *                               active_db - hysteresis_db (a hovering drone
 *                               rings once, not every refractory period)
 * A channel without a sample for stale_ms restarts from IDLE (its smoothing
 * too); a running refractory period is kept. Times are uint32 milliseconds
 * and may wrap. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PD_CHANNELS 8u   /* R1..R8, or channel 1..8 of any band */

typedef enum { PD_IDLE, PD_RISING, PD_PEAK_PASSED, PD_COOLDOWN } pd_state_t;

typedef struct {
    float alpha;             /* smoothing: weight of a new sample, (0, 1] */
    float active_db;         /* activity threshold (RISING above it) */
    float hysteresis_db;     /* release = active_db - hysteresis_db */
    float drop_db;           /* drop under the maximum that marks the pass */
    float min_peak_db;       /* far drones (lower maximum) never ring */
    uint32_t refractory_ms;  /* no second event on the channel before this */
    uint32_t stale_ms;       /* no sample this long: back to IDLE */
} pd_config_t;

typedef struct {
    uint8_t channel;
    float peak_db;           /* smoothed maximum of the pass */
    uint32_t peak_ms;        /* when the maximum was reached (the pass) */
    uint32_t at_ms;          /* when the drop confirmed it (the event) */
} pd_event_t;

typedef struct {
    pd_state_t state;
    bool primed;             /* smooth holds a value */
    float smooth, peak;
    uint32_t peak_ms, last_ms, cooldown_until_ms;
} pd_channel_t;

typedef struct {
    pd_config_t cfg;
    pd_channel_t ch[PD_CHANNELS];
} pass_detector_t;

/* alpha 0.3 (time constant ~150 ms at 20 Hz), active 15 dB, hysteresis 3 dB,
 * drop 3 dB, min peak 25 dB, refractory 1.5 s, stale 1 s. */
pd_config_t pd_config_default(void);
/* Ranges that keep the state machine meaningful (alpha in (0,1], positive
 * hysteresis and drop, min_peak_db >= active_db, stale_ms > 0). */
bool pd_config_valid(const pd_config_t *cfg);

void pd_init(pass_detector_t *d, const pd_config_t *cfg);
/* New thresholds; channel states are kept. False (nothing changed) when invalid. */
bool pd_set_config(pass_detector_t *d, const pd_config_t *cfg);
void pd_reset_channel(pass_detector_t *d, unsigned channel);
void pd_reset_all(pass_detector_t *d);

/* One sample of `channel`. True when it completes a pass: *event filled. */
bool pd_update(pass_detector_t *d, unsigned channel, float level_db, uint32_t now_ms, pd_event_t *event);
/* Stale channels back to IDLE without a sample (call when none is available). */
void pd_expire(pass_detector_t *d, uint32_t now_ms);

const char *pd_state_name(pd_state_t state);

#ifdef __cplusplus
}
#endif
