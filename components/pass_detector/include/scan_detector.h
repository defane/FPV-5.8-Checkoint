/* Scan detector: "a drone has just passed" on any of R1..R8, from the IQ power of
 * one sweep of the band. Pure C, no hardware.
 *
 * Every channel has its own resting level, in dB: its noise differs (13 dB across
 * the band on the board) and the power is on no absolute scale. The rise above it
 * goes to pass_detector, one channel per slot, so pd_config_t reads in dB of rise.
 *
 * The rest follows the channel's low values, faster for its first seconds, stops
 * climbing while the channel is active (or a VTX left on would sink slowly, which
 * reads as a pass) and may not climb more than SD_REST_SPREAD_DB above the other
 * channels' median: a VTX left on keeps showing, and a drone waiting by the gate is
 * not learnt as the channel's rest, or its first laps would rise from nothing.
 * Nothing rings while the rests settle, over the first SD_LEARN_SWEEPS. Under SD_SIGNAL_DB the 4-bit IQ
 * only flickers, so a channel has to reach it to count, whatever its rest. */
#pragma once
#include "pass_detector.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_NO_DATA        0xffffu     /* a window that could not be read */
#define SD_FLOOR_DB       (-30.0f)    /* power 0 after the DC removal */
#define SD_SIGNAL_DB      (-5.0f)
#define SD_REST_SPREAD_DB 6.0f
#define SD_REST_UP_DB     0.002f      /* per reading: 0.1 dB/s at 57 Hz */
#define SD_REST_DOWN_DB   0.02f       /* 1 dB/s: settles on its low values */
#define SD_LEARN_SWEEPS   170u        /* 3 s at 57 Hz, ten times faster */
#define SD_LEARN_RATE     10.0f

typedef struct {
    pass_detector_t det;
    float rest_db[PD_CHANNELS];
    bool primed[PD_CHANNELS];
    uint32_t sweeps;
} scan_detector_t;

/* alpha 0.25 per sweep, rise over 8 dB is active, 3 dB hysteresis and drop, min peak
 * 10 dB (weak bumps reach +8 on the board, hand passes +13 to +16), refractory 1.5 s,
 * stale 1 s. */
pd_config_t sd_config_default(void);
void sd_init(scan_detector_t *sd, const pd_config_t *cfg);

/* Power as window_power() gives it, x256, in dB. */
float sd_power_db(uint32_t power);

/* One sweep, power[ch] (SD_NO_DATA: no reading). Fills events[] with the passes it
 * completes, PD_CHANNELS at most, and returns how many. */
unsigned sd_sweep(scan_detector_t *sd, const uint16_t power[PD_CHANNELS], uint32_t now_ms, pd_event_t *events);

/* How many dB a channel stands above its rest, smoothed. */
float sd_rise_db(const scan_detector_t *sd, unsigned channel);
/* The channel standing highest. */
unsigned sd_strongest(const scan_detector_t *sd);

#ifdef __cplusplus
}
#endif
