/* Passage calibration: the detector's thresholds from two measurements with a real drone, one in
 * the passage and one 2 m away from it. Pure C, no hardware.
 *
 * The scan task samples each channel's smoothed rise (sd_rise_db) once per rotation for a few
 * seconds into a histogram (pc_hist_t, 2 KB: the C5 allocates it on the heap for the measurement).
 * The drone is the channel that stands highest in the passage; the second measurement reads the
 * same channel. pc_solve() turns the two into the thresholds that put a pass between what the
 * drone reads away from the gate and what it reads in it.
 *
 * The rules are a first proposal (docs/detector.md "Passage calibration"), to be confirmed on
 * real passes (bench B6): a hover overstates a real pass's smoothed peak (tau is about 61 ms, a
 * drone crosses the passage in 0.1 to 0.2 s), so min_peak sits a fixed margin above the away
 * level rather than halfway to the hover. D (the re-arm drop) and alpha belong to the track
 * and the filter, not to the gate: they are left alone. All levels are dB of rise above a
 * channel's rest; the wire and the stats keep them in half dB (x2), the thresholds in tenths. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_CHANNELS 8u
#define PC_BINS     128u
#define PC_BIN_DB   0.5f
#define PC_BASE_DB  (-8.0f)             /* the lower edge of bin 0; rises outside the range count in the end bins */

typedef struct {
    uint16_t bin[PC_CHANNELS][PC_BINS]; /* samples per rise bin, saturating at 65535 */
    uint32_t caps, sat_caps;            /* captures seen, and those that hit an ADC rail */
} pc_hist_t;

void pc_hist_clear(pc_hist_t *h);
void pc_hist_add(pc_hist_t *h, unsigned channel, float rise_db);   /* one sample of a channel */
void pc_hist_capture(pc_hist_t *h, bool saturated);                 /* one capture, clipping or not */

/* A channel's rise over the measurement, half dB (x2), -64..63.5 dB. */
typedef struct {
    uint8_t channel;
    int8_t  p10, p50, p90, p95, max;
    uint8_t sat_pct;                    /* captures that clipped, % (0..100) */
    uint16_t samples;                   /* the channel's samples, saturating at 65535 */
} pc_stats_t;

void pc_stats(const pc_hist_t *h, unsigned channel, pc_stats_t *out);
unsigned pc_strongest(const pc_hist_t *h);   /* the channel with the highest p95 (the lowest one on a tie) */

/* Warnings of pc_solve (pc_result_t.flags). */
#define PC_NO_DRONE        0x01u   /* in the passage the drone's p95 stands under PC_SEEN_DB: nothing was measured */
#define PC_CLIPPING        0x02u   /* the passage measurement clipped on at least PC_CLIP_PCT % of its captures */
#define PC_LOW_SEPARATION  0x04u   /* the drone's p10 in the passage is under its away p95 by less than PC_SEPARATION_DB */
#define PC_AWAY_WEAK       0x08u   /* away, the drone's p95 stands under PC_AWAY_SEEN_DB: the gain is low or it was not on */
#define PC_OTHER_CHANNEL   0x10u   /* away, another channel stands 6 dB over the drone's: not the same drone */
#define PC_BUSY            0x20u   /* the C5 refused the measurement: the rests are being learnt, a calibration or a bench runs */

#define PC_SEEN_DB         6.0f
#define PC_CLIP_PCT        5u
#define PC_SEPARATION_DB   6.0f
#define PC_AWAY_SEEN_DB    3.0f
#define PC_MARGIN_DB       2.5f    /* min_peak over the away p95 */
#define PC_MIN_PEAK_LO_DB  8.0f    /* active_db (min_peak - 2) never under 6 dB */
#define PC_MIN_PEAK_HI_DB  40.0f

typedef struct {
    uint16_t flags;                 /* PC_* */
    int16_t  active_db_x10, hysteresis_db_x10, drop_db_x10, min_peak_db_x10;   /* the thresholds proposed, dB x10 */
    int8_t   gain_steps;            /* advice: -1 lower the gain by one step (2) when clipping, +1 raise it when the drone is
                                     * hardly seen away; 0 otherwise. The calibration never changes it. */
    bool     valid;                 /* thresholds proposed (false with PC_NO_DRONE: they are the detector defaults) */
} pc_result_t;

/* gate: the drone's channel in the passage; away: the same channel 2 m away; away_top: the channel that
 * stood highest away. */
void pc_solve(const pc_stats_t *gate, const pc_stats_t *away, const pc_stats_t *away_top, pc_result_t *out);

#ifdef __cplusplus
}
#endif
