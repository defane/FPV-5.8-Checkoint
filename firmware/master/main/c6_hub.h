/* C6 fleet hub: owns the ESP-NOW link to the C3 nodes, the paired-node table
 * (NVS), the fleet time beacons, and the race_timing engine. Gate 0 is the
 * master's own local C5; remote nodes are gates 1.. assigned at pairing.
 * Every pass (local or remote) is timed and sounded centrally. */
#pragma once
#include "link_proto.h"
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
    int8_t   levels[LP_LEVELS];   /* last reported */
    uint8_t  state;          /* pd_state_t of the node's detector */
    uint32_t last_seen_ms;
    bool     online;         /* seen within the liveness window */
    int8_t   rssi;           /* last ESP-NOW RSSI dBm; 0 = none yet */
    uint32_t rx_frames;      /* total fleet frames received from this node */
    uint32_t status_seq;     /* last NODE_STATUS seq seen */
    uint32_t status_rx;      /* NODE_STATUS received since last (re)sync */
    uint32_t status_lost;    /* NODE_STATUS inferred lost (seq gaps) */
    bool     seq_init;       /* status_seq has been primed */
    /* The node's C5 health, from its last NODE_STATUS (zero/empty until then). */
    uint32_t passes;         /* passes the C5 detected since its boot */
    uint8_t  flags;          /* LP_HEALTH_* */
    uint8_t  gain;           /* RX gain index */
    uint32_t c5_uptime_s;
    uint32_t heap_min;       /* lowest free internal heap since boot, bytes */
    uint32_t faults;         /* transport fault counters */
    char     fw[LP_FW_LEN];  /* firmware id */
} c6_node_t;

typedef struct {
    uint8_t  channel;
    uint32_t laps, last_lap_ms, best_lap_ms;
    int16_t  last_gate;
    uint32_t last_seen_ms;   /* fleet clock at its last pass, comparable to uptime_ms */
} c6_standing_t;

typedef struct {
    bool          pairing;
    uint8_t       num_gates;
    uint8_t       num_nodes;
    uint8_t       volume;        /* centralized audio volume, 0..100 % */
    uint32_t      uptime_ms;     /* master uptime */
    uint32_t      free_heap;     /* master free internal heap, bytes */
    uint32_t      heap_total;    /* master total internal heap, bytes */
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
bool c6_hub_reset_channel(uint8_t channel);
void c6_hub_results(race_timing_t *out);   /* copy of the timing state */
void c6_hub_snapshot(c6_fleet_t *out);
/* Run a C5 command (calibrate/reboot) at a gate: 0 is the local C5 over UART
 * (false while it is absent: no STATS for C6_LOCAL_LIVE_MS), 1.. its online
 * node(s) over ESP-NOW (false if none). The caller confirms the VTXs are off
 * before a calibration. */
bool c6_hub_send_command(uint8_t gate_index, lp_cmd_t cmd);
int32_t c6_hub_vtx_threshold_x10(void);        /* remote gates: C/N*10 above which a VTX is probably on (pushed active_db) */
