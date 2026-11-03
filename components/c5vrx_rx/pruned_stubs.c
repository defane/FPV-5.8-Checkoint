/* C5VRX-5 by Twotoz and the C5VRX contributors: inert stand-ins for the
 * upstream modules the FPV 5.8 Checkpoint fork drops (C5VRX commit 6bc9e4b).
 * Each block keeps the exact behaviour the kept code saw before, with the
 * video output off and no flight log partition:
 *  - experimental.c + experimental_phy.c: every field A/B control at its
 *    baseline (index 0), so no PHY patch is ever written; the legacy choices
 *    kept elsewhere (gain mode, digital filter, AFC, decoder, auto profile)
 *    still report their live state, so T's "EXP active" line is unchanged;
 *  - flightlog.c: logging off, as upstream without a "flightlog" partition;
 *  - sense_lab.c and calibration_bench.c: bench tests that were only ever
 *    started from the console (S, L, N), never queued here. */
#include "receiver.h"
#include "experimental.h"
#include "sense_lab.h"
#include "phy.h"
#include "rf.h"
#include <string.h>

/* ---- experimental.c / experimental_phy.c: baseline only ---- */
uint8_t experimental_get(unsigned item) { (void)item; return 0; }
void experimental_load(void) {}
bool experimental_set(unsigned item, uint8_t value) { (void)item; return value == 0; }
void experimental_reset(void) {}
/* Upstream's snapshot and mask with every experiment at 0: the legacy
 * choices still come from their owners. */
void experimental_snapshot(uint8_t values[EXP_ITEMS])
{
    memset(values, 0, EXP_ITEMS);
    values[EXP_GAIN] = receiver_gain_mode();
    values[EXP_DFILT] = (uint8_t)phy_dfilt_mode();
    values[EXP_AFC] = !s_afc_enabled;
    values[EXP_DEMOD] = transport_demod();
    values[EXP_AUTO_PROFILE] = profile_live_mode();
}
uint32_t experimental_mask(void)
{
    uint8_t v[EXP_ITEMS];
    experimental_snapshot(v);
    uint32_t mask = 0;
    for (unsigned i = 0; i < EXP_ITEMS; ++i) {
        const unsigned baseline = i == EXP_GAIN ? GAIN_MODE_DEFAULT : i == EXP_DFILT ? PHY_DFILT_MODE_DEFAULT : 0;
        if (v[i] != baseline) mask |= 1u << i;
    }
    if (rf_lane() != RF_LANE_ULTRAFINE) mask |= 1u << EXP_IQ_LANE;
    return mask;
}
void experimental_log_state(void) {}
void experimental_poll(void) {}
unsigned experimental_boot_lane(void) { return RF_LANE_ULTRAFINE; }
bool experimental_reboot_needed(void) { return rf_lane() != RF_LANE_ULTRAFINE; }
bool experimental_phy_restore(void) { return true; }
void experimental_phy_prepare_restore(void) {}
/* Upstream: any PHY experiment set, or a lane other than ultrafine. With
 * every experiment at baseline only the lane is left. Not a constant: it
 * gates C/N, the gain learn and every calibration (receiver.c, calibration*.c). */
bool experimental_transfer_active(void) { return rf_lane() != RF_LANE_ULTRAFINE; }

/* ---- flightlog.c: no partition, nothing recorded ---- */
void flightlog_record(uint8_t type, const void *payload, uint8_t len) { (void)type; (void)payload; (void)len; }
void flightlog_event(uint8_t kind, const void *data, uint8_t len) { (void)kind; (void)data; (void)len; }
void flightlog_second(const diag_record_t *r, uint8_t gain_min, uint8_t gain_max, uint8_t dropout, uint8_t rx_state)
{
    (void)r; (void)gain_min; (void)gain_max; (void)dropout; (void)rx_state;
}
void flightlog_trace_add(const diag_record_t *r, unsigned k, unsigned count) { (void)r; (void)k; (void)count; }
void flightlog_snapshot_request(uint8_t trigger) { (void)trigger; }

/* ---- sense_lab.c: no request is ever queued ---- */
bool sense_lab_pending(void) { return false; }
void sense_lab_run(bool quiet) { (void)quiet; }

/* ---- calibration_bench.c (calibration_internal.h): never requested ---- */
extern volatile bool s_lane_requested, s_selfnoise_requested;
void lane_compare(void);
void self_noise_test(void);
volatile bool s_lane_requested, s_selfnoise_requested;
void lane_compare(void) {}
void self_noise_test(void) {}
