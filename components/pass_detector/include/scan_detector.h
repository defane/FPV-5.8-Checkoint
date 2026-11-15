/* Scan detector: "a drone has just passed" on any of R1..R8, from per-channel band
 * levels read at any rate (about 500 a second per channel with sdr_rx), each with its
 * own time. Pure C, no hardware.
 *
 * Every channel has its own resting level, in dB: its noise differs across the band
 * and the level is on no absolute scale. The rise above it goes to pass_detector, one
 * channel per slot, so pd_config_t reads in dB of rise.
 *
 * Per channel: a median of the last 3 readings (a lone burst reading vanishes), then an
 * exponential average whose weight comes from each reading's interval, so the time
 * constant is the same at any rate. alpha keeps its meaning: the weight per reference
 * sweep of SD_REF_SWEEP_US (the 57 Hz the defaults were tuned at), i.e. a time constant
 * tau = -SD_REF_SWEEP_US / ln(1 - alpha). pass_detector then gets the smoothed rise
 * with weight 1.
 *
 * The rest follows the channel's low smoothed values (rates per SD_REF_SWEEP_US, scaled
 * by each reading's interval), faster for the first SD_LEARN_US, stops climbing while
 * the channel is active (or a VTX left on would sink slowly, which reads as a pass) and
 * may not climb more than SD_REST_SPREAD_DB above the other channels' median: a VTX left
 * on keeps showing, and a drone waiting by the gate is not learnt as the channel's rest,
 * or its first laps would rise from nothing. A saturated capture never moves a rest
 * (compression reads the partner channel low). Nothing rings while the rests settle.
 * A channel without a reading for over SD_DT_MAX_US (a calibration) restarts its median
 * and its average from the next reading. */
#pragma once
#include "pass_detector.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_LEVEL_NONE     INT16_MIN   /* no reading (= SDR_LEVEL_NONE) */
#define SD_REF_SWEEP_US   17500u      /* alpha, SD_REST_UP/DOWN_DB are per this interval */
#define SD_REST_SPREAD_DB 6.0f
#define SD_REST_UP_DB     0.002f      /* per 17.5 ms: 0.114 dB/s */
#define SD_REST_DOWN_DB   0.02f       /* per 17.5 ms: 1.14 dB/s, settles on its low values */
#define SD_LEARN_US       3000000u    /* the rests settle ten times faster for 3 s */
#define SD_LEARN_RATE     10.0f
#define SD_DT_MAX_US      100000      /* a longer gap re-primes the channel */
#define SD_FLAG_SATURATED 0x01u       /* sd_feed flags: = SDR_RD_SATURATED */

typedef struct {
    int16_t hist[3];                  /* median-of-3 input, level codes */
    uint8_t nhist;                    /* valid entries, saturates at 3 (never wraps) */
    uint8_t hidx;                     /* next write slot, cycles 0..2 */
    bool primed;
    float smooth_db;                  /* average of the level, dB */
    int64_t last_us;                  /* time of the last reading */
    int32_t dt_avg_us;                /* average reading interval, 0 = none yet */
    float rest_db;
    bool rest_primed;
} sd_channel_t;

typedef struct {
    pass_detector_t det;              /* fed the smoothed rise with weight 1 */
    sd_channel_t c[PD_CHANNELS];
    float tau_us;                     /* time constant from alpha, 0 = no smoothing */
    float cap_db;                     /* median of primed rests + SD_REST_SPREAD_DB; +inf before the first rotation */
    int64_t learn_end_us;             /* 0 = set by the next reading */
} scan_detector_t;

/* alpha 0.25 per 17.5 ms (tau 60.8 ms), rise over 8 dB is active, 3 dB hysteresis and
 * drop, min peak 10 dB, refractory 1.5 s, stale 1 s. */
pd_config_t sd_config_default(void);
void sd_init(scan_detector_t *sd, const pd_config_t *cfg);
/* New thresholds; channel states are kept, tau follows alpha. False (nothing changed)
 * when invalid. */
bool sd_set_config(scan_detector_t *sd, const pd_config_t *cfg);
/* Forgets the rests, the smoothing and the pd states: learning restarts. */
void sd_relearn(scan_detector_t *sd);

/* One reading of `channel`: level_qdb in 0.25 dB steps (SD_LEVEL_NONE: none, changes
 * nothing), flags SD_FLAG_*, t_us its time (monotonic, microseconds). True when it
 * completes a pass: *event filled, its times in ms of t_us, peak_ms the smoothed peak. */
bool sd_feed(scan_detector_t *sd, unsigned channel, int16_t level_qdb, uint8_t flags, int64_t t_us,
             pd_event_t *event);
/* Once per rotation (every channel read once): the rests' cap from their median, and
 * channels without a reading for pd's stale_ms back to IDLE. */
void sd_rotation(scan_detector_t *sd, int64_t t_us);
/* Nothing rings: before the first reading and for SD_LEARN_US after it. */
bool sd_learning(const scan_detector_t *sd, int64_t t_us);

/* How many dB a channel stands above its rest, smoothed. */
float sd_rise_db(const scan_detector_t *sd, unsigned channel);
/* The channel standing highest. */
unsigned sd_strongest(const scan_detector_t *sd);
/* The smoothing's time constant, ms. */
float sd_tau_ms(const scan_detector_t *sd);
/* The smoothing delay of a channel's peak, ms: (tau_us + dt_avg_us) / 1000, the average
 * plus the median's one reading. An estimate for the console; peak_ms stays uncorrected. */
float sd_delay_ms(const scan_detector_t *sd, unsigned channel);

/* Legacy sweep API for the C5VRX glue (components/checkpoint until it moves to sdr_rx,
 * which deletes this block). A sweep is one reading per channel, all at now_ms. */
#define SD_NO_DATA 0xffffu            /* a window that could not be read */
/* Power as window_power() gives it, x256, in dB (-30 dB floor). */
float sd_power_db(uint32_t power);
/* sd_feed of round(4 * sd_power_db(power[ch])) per channel (SD_NO_DATA skipped), flags 0,
 * t = now_ms * 1000, then sd_rotation. Fills events[] (PD_CHANNELS at most), returns how many. */
unsigned sd_sweep(scan_detector_t *sd, const uint16_t power[PD_CHANNELS], uint32_t now_ms, pd_event_t *events);

#ifdef __cplusplus
}
#endif
