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
 * and holds its next readings until the median is full again (a lone burst after the gap
 * stays a lone burst); its average then resumes where it was or, after a gap longer than
 * pd's stale_ms (pd has forgotten the channel), starts again from that median. A late
 * reading (over twice the usual interval) weighs in the average as one on time.
 *
 * Relative re-arm (pd_set_rearm(&sd->det, drop), pass_detector.h; off after sd_init): a channel
 * re-armed by it is not quiet, so its rest does not climb, and a channel back from a long hold of
 * the pair guard is relatively armed at the level it comes back at (pd_resume).
 *
 * Pair guard (sd_feed_hop, the readings of one capture). In PAIRS mode one capture carries two
 * channels 37 MHz apart, each in the other's image window. While a strong channel drives the ADC
 * into its rails (SD_FLAG_SATURATED, one flag per capture), the I/Q correction no longer holds and
 * the rails' own products land in the partner's window: the partner reads a level made by the
 * strong channel (host model: a median 23-29 dB under it, a few % of captures above it) and rang
 * phantom passes on the bench. So on a saturated capture (and, within a saturated run that has
 * lasted SD_HOLD_HOVER_US, until SD_HOLD_TAIL_US after its last saturated capture), the reading of
 * a channel whose smoothed level stands more than SD_HOLD_DOM_DB under its partner's is held: not
 * fed, its state frozen. The comparison is on the smoothed levels, not on one capture's readings:
 * the products top their source on a few % of captures. Except after a jump: a VTX switched on or
 * armed beside the antenna clips while its average is still low (at the floor, or more than
 * SD_HOLD_DOM_DB under a partner that stands up), and the smoothed levels alone would hold the
 * source itself, frozen low for the whole run, and feed its products to the partner. So when the
 * last 3 readings of a channel all stand more than SD_HOLD_DOM_DB over its partner's median and
 * more than SD_HOLD_JUMP_DB over its own average, it is the run's source: its partner is held until
 * the saturated run ends. A drone flown in never leads its average that much (host model), so
 * there both rules hold the same channel. When a held channel is fed again, its median
 * restarts on fresh readings; if it stood min_peak_db up as its hold began (a real pass under way),
 * its average catches up to the fresh median less drop_db (never down, so a catch-up never rings by
 * itself). After a hold longer than SD_HOLD_LONG_US, what it did while blind is unknown: once its
 * median is full, its average and pd start again from that median, whatever the hold's length
 * (pd_resume: armed under the release; over it relatively armed with that level as its trough, or
 * in COOLDOWN with the re-arm off). The tail of a pass it missed never rises the re-arm's drop over
 * the level it came back at, so it rings nothing; its next pass does. Unsaturated captures go
 * through exactly as sd_feed. */
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
#define SD_DT_MAX_US      100000      /* a longer gap restarts the channel's median */
#define SD_FLAG_SATURATED 0x01u       /* sd_feed flags: = SDR_RD_SATURATED */
#define SD_HOP_READINGS   2u          /* readings of one capture, at most (= SDR_RX_PER_HOP) */
#define SD_HOPS_MAX       8u          /* hops of a plan, at most (= SDR_RX_MAX_HOPS) */
#define SD_HOLD_DOM_DB    15.0f       /* a saturated capture's reading is held this far under its partner */
#define SD_HOLD_JUMP_DB   20.0f       /* a channel's last 3 readings this far over its average: it jumped */
#define SD_HOLD_TAIL_US   30000       /* a saturated run: saturated captures this close; a hover's tail */
#define SD_HOLD_HOVER_US  150000      /* a run this long is a hover (a pass clips ~80 ms at 0.3 m) */
#define SD_HOLD_LONG_US   500000      /* after a longer hold the channel starts again (pd_resume) */

/* One reading of a capture (the fields of sdr_reading_t). */
typedef struct {
    int64_t t_us;
    int16_t level_qdb;
    uint8_t channel;
    uint8_t flags;
} sd_reading_t;

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
    /* pair guard (sd_feed_hop) */
    int64_t sat_run_us[SD_HOPS_MAX];  /* per hop: start of its saturated run */
    int64_t sat_last_us[SD_HOPS_MAX]; /* per hop: its last saturated capture, 0 = none */
    uint8_t sat_src[SD_HOPS_MAX];     /* per hop: 1 + the reading index a jump showed to be the source, this run */
    int16_t raw[PD_CHANNELS][3];      /* each channel's last 3 readings into sd_feed_hop, held ones too */
    uint8_t raw_n[PD_CHANNELS];       /* valid entries of raw, saturates at 3 */
    uint8_t raw_i[PD_CHANNELS];       /* next write slot of raw, cycles 0..2 */
    int64_t held_since_us[PD_CHANNELS];
    float held_rise_db[PD_CHANNELS];  /* pd's smoothed rise as the hold began */
    uint8_t held_mask;                /* channels whose last reading was held */
    uint8_t fresh_mask;               /* channels catching up once their median is full again */
    uint8_t long_mask;                /* channels back from a long hold, restarting once it is full */
    uint32_t held[PD_CHANNELS];       /* per channel, readings held since sd_init (diagnostics) */
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
/* The n (<= SD_HOP_READINGS) readings of one capture of plan hop `hop`, through the pair guard,
 * then sd_feed. Returns a bit per reading that completed a pass (ev[i] filled); *held (may be
 * NULL) gets a bit per reading held (not fed). Without SD_FLAG_SATURATED on the capture (and
 * outside a hover's tail) it is sd_feed of each reading in order. */
unsigned sd_feed_hop(scan_detector_t *sd, unsigned hop, const sd_reading_t *r, unsigned n, pd_event_t *ev,
                     unsigned *held);
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

#ifdef __cplusplus
}
#endif
