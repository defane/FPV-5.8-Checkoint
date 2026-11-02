/* C6 fleet hub: owns the ESP-NOW link to the C3 nodes, the paired-node table
 * (NVS), the fleet time beacons, and the race_timing engine. Gate 0 is the
 * master's own local C5; remote nodes are gates 1.. assigned at pairing.
 * Every pass (local or remote) is timed and sounded centrally. */
#pragma once
#include "race_timing.h"
#include <stdbool.h>
#include <stdint.h>

#define C6_MAX_NODES (RT_MAX_GATES - 1u)   /* remote gates 1..; gate 0 is local */

typedef struct {
    uint8_t  mac[6];
    uint8_t  node_id;
    uint8_t  gate_index;     /* 1.. */
    uint8_t  channel;        /* assigned FPV channel code */
    int32_t  cn_x10;         /* last reported; LP_CN_UNKNOWN if none */
    uint8_t  state;
    uint32_t last_seen_ms;
    bool     online;         /* seen within the liveness window */
} c6_node_t;

typedef struct {
    uint8_t  channel;
    uint32_t laps, last_lap_ms, best_lap_ms;
    int16_t  last_gate;
} c6_standing_t;

typedef struct {
    bool          pairing;
    uint8_t       num_gates;
    uint8_t       num_nodes;
    uint8_t       volume;        /* centralized audio volume, 0..100 % */
    uint32_t      uptime_ms;     /* master uptime */
    uint32_t      free_heap;     /* master free internal heap, bytes */
    c6_node_t     nodes[C6_MAX_NODES];
    uint8_t       num_standings;
    c6_standing_t standings[RT_MAX_CHANNELS];
} c6_fleet_t;

void c6_hub_start(void);                       /* after Wi-Fi AP is up */
void c6_hub_local_pass(uint8_t channel, int32_t peak_db_x10, uint32_t at_ms);
void c6_hub_set_pairing(bool open);
void c6_hub_set_volume(uint8_t pct);           /* 0..100 %, applied + saved in NVS */
bool c6_hub_set_node(uint8_t node_id, uint8_t gate_index, uint8_t channel);  /* reassign */
bool c6_hub_remove_node(uint8_t node_id);
void c6_hub_reset_race(void);
void c6_hub_snapshot(c6_fleet_t *out);
