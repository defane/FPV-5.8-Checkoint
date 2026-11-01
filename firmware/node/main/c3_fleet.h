/* C3 fleet uplink: ESP-NOW to the C6 master. Pairs, applies the C6's CONFIG
 * (detector + channel retune) to the local C5, relays the C5's passes as
 * fleet-timed GATE_PASS and periodic NODE_STATUS, and tracks fleet time from
 * the master's beacons. Wi-Fi (STA) and espnow must be up before use. */
#pragma once
#include "esp_err.h"
#include "link_proto.h"
#include <stdbool.h>
#include <stdint.h>

void c3_fleet_init(void);                         /* load saved C6 + assignment from NVS */
esp_err_t c3_fleet_start_espnow(uint8_t channel); /* on the AP interface; after Wi-Fi is up */
/* Probe the master (pair if needed) for up to timeout_ms; true if reached. */
bool c3_fleet_try_online(uint32_t timeout_ms);
void c3_fleet_report_pass(const lp_event_t *e);   /* relay a local C5 pass to the C6 */
void c3_fleet_report_stats(const lp_stats_t *s);  /* relay periodic status to the C6 */
