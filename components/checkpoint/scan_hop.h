/* The Raceband sweep: a fast hop over R1-R8 with one IQ power window per channel.
 * The detector runs it all the time (checkpoint.c), the console's benches on demand
 * (hop_bench.c). Numbers and why they are what they are: docs/bench.md. */
#pragma once
#include "receiver.h"
#include "scan_detector.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCAN_CHANNELS     PD_CHANNELS   /* R1..R8 are indices 0..7 */
#define SCAN_BASE_CHANNEL 3u            /* R4: mid band, the Wi-Fi centre direct hops stay on */
#define SCAN_FRESH_US     1000u         /* what a strong channel leaves is gone by 1 ms */
#define SCAN_YIELD_SWEEPS 16u           /* lets idle and priority 1 run */
#define SCAN_GAIN_DEFAULT 24u           /* onboard antenna, close passes */
#define SCAN_GAIN_MIN     24u           /* the console's fixed gains */
#define SCAN_GAIN_MAX     78u
#define SCAN_GAIN_STEP    4u

extern const uint8_t scan_order[SCAN_CHANNELS];   /* crosses the band: R1 R5 R2 R6 R3 R7 R4 R8 */

typedef struct { size_t channel; int offset_khz; uint8_t gain; } scan_saved_t;

uint8_t scan_gain(void);
void scan_gain_step(bool up);                     /* not saved */

/* Nothing else owns the receiver: no calibration, no pause, no other scan. */
bool scan_receiver_free(void);
/* Owns the receiver like scan() does (receiver_pause + s_scan_active, so the control
 * loop stops reading windows and steering AFC), at one fixed gain. */
bool scan_take(const char *tag, uint8_t gain, scan_saved_t *saved);
/* Gives it back on the saved channel, offset and gain; returns the retunes that failed. */
unsigned scan_give(const scan_saved_t *saved);

/* One sweep, power[ch] as hs_window_power() or SD_NO_DATA. window: CONTROL_BYTES. */
void scan_sweep(uint8_t *window, uint16_t power[SCAN_CHANNELS], uint32_t *no_data);
