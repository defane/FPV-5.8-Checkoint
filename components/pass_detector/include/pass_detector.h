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
 *                               OR, with a re-arm drop (pd_set_rearm, 0 = off),
 *                               refractory_ms elapsed AND the level's 1 s average
 *                               (PD_REARM_TAU_MS) that drop under the highest
 *                               level since the pass: the relative re-arm
 * The relative re-arm is for a drone that never falls under the release between
 * laps (a hot link, a small track). After it, a pass must also rise the drop over
 * the trough, the lowest 1 s average since the re-arm: IDLE -> RISING needs the
 * level above both active_db and trough + drop, so the rest of the fall and a
 * wobble at a high level ring nothing. Falling under the release ends it: the
 * channel is then armed as without it.
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
#define PD_REARM_TAU_MS 1000u   /* the relative re-arm's average: a dip shorter than this is a fade */

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
    bool rel;                /* it followed a relative re-arm */
} pd_event_t;

typedef struct {
    pd_state_t state;
    bool primed;             /* smooth holds a value */
    bool rel;                /* armed by the relative re-arm (until the level falls under the release) */
    float smooth, peak;      /* peak: the pass's maximum; in COOLDOWN, with a re-arm drop, the highest level since */
    float slow;              /* smooth averaged over PD_REARM_TAU_MS */
    float trough;            /* after a relative re-arm: the lowest slow since */
    uint32_t peak_ms, last_ms, cooldown_until_ms;
} pd_channel_t;

typedef struct {
    pd_config_t cfg;
    float rearm_db;          /* the relative re-arm's drop, dB; 0 = off (pd_set_rearm) */
    pd_channel_t ch[PD_CHANNELS];
} pass_detector_t;

/* alpha 0.3 (time constant ~150 ms at 20 Hz), active 15 dB, hysteresis 3 dB,
 * drop 3 dB, min peak 25 dB, refractory 1.5 s, stale 1 s. */
pd_config_t pd_config_default(void);
/* Ranges that keep the state machine meaningful (alpha in (0,1], positive
 * hysteresis and drop, min_peak_db >= active_db, stale_ms > 0). */
bool pd_config_valid(const pd_config_t *cfg);

/* Relative re-arm off; every channel reset. */
void pd_init(pass_detector_t *d, const pd_config_t *cfg);
/* The relative re-arm's drop in dB (0, negative or NaN: off, the re-arm is the release
 * alone). Channel states are kept. Not part of pd_config_t: the build sets it
 * (CONFIG_CHECKPOINT_REARM_DROP_DB), the saved settings and the link do not carry it. */
void pd_set_rearm(pass_detector_t *d, float drop_db);
/* New thresholds; channel states are kept. False (nothing changed) when invalid. */
bool pd_set_config(pass_detector_t *d, const pd_config_t *cfg);
void pd_reset_channel(pass_detector_t *d, unsigned channel);
void pd_reset_all(pass_detector_t *d);

/* One sample of `channel`. True when it completes a pass: *event filled. */
bool pd_update(pass_detector_t *d, unsigned channel, float level_db, uint32_t now_ms, pd_event_t *event);
/* The same with the smoothing weight of this sample instead of cfg.alpha (a caller that
 * smooths itself, or weighs by the sample's interval). weight >= 1: smooth = level_db
 * exactly. */
bool pd_update_w(pass_detector_t *d, unsigned channel, float level_db, float weight, uint32_t now_ms,
                 pd_event_t *event);
/* Stale channels back to IDLE without a sample (call when none is available). */
void pd_expire(pass_detector_t *d, uint32_t now_ms);

const char *pd_state_name(pd_state_t state);

#ifdef __cplusplus
}
#endif
