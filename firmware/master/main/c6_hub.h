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
    uint16_t states;         /* each channel's detector state, lp_state_of() (all IDLE until told) */
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
    uint32_t faults;         /* capture faults (timeouts, short/overrun dumps, DSP errors)
                              * + scan stalls, since its boot */
    uint16_t rearm_x10;      /* the re-arm drop its C5 echoed *10, FP_REARM_UNKNOWN until told */
    char     fw[LP_FW_LEN];  /* firmware id */
    lp_calib_t calib;        /* the C5's passage-calibration state, last relayed */
    uint32_t calib_ms;       /* when it came, 0 = none yet or the node rebooted since */
    bool     custom;         /* a snapshot's: this node has thresholds from a passage calibration (not the fleet's) */
} c6_node_t;

typedef struct {
    uint8_t  channel;
    uint32_t laps, last_lap_ms, best_lap_ms;
    int16_t  last_gate;
    uint32_t last_seen_ms;   /* fleet clock at its last pass, comparable to uptime_ms */
} c6_standing_t;

typedef struct {
    bool          pairing;
    uint16_t      pairing_left_s;   /* seconds until an open pairing closes by itself, 0 when closed */
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
/* Pairing is closed at every boot. true opens it for 5 minutes (again from now if already open), false closes it. */
void c6_hub_set_pairing(bool open);
void c6_hub_set_volume(uint8_t pct);           /* 0..100 %, applied + saved in NVS */
/* The fleet's relative re-arm drop *10 (0..LP_REARM_MAX_X10, else false): saved (c6_store), sent
 * now to every online remote gate in its CONFIG (and from then on at pairing, a gate's save and
 * back online) and to gate 0 (c6_link_sync_rearm). */
bool c6_hub_set_rearm(int32_t x10);
/* The order of the remote gates: ids lists every paired node's id, first gate first; they get gates 1..N in that
 * order (gate 0 stays the local C5), each online one its new CONFIG. False, nothing changed, unless ids is a
 * permutation of all the paired nodes' ids (the page's list may be stale). A change restarts the race timing; the
 * same order changes nothing. */
bool c6_hub_set_order(const uint8_t *ids, unsigned n);
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
/* Force a gate's C5 RX gain (LP_GAIN_MIN..LP_GAIN_MAX): 0 is the local C5 over UART, 1.. its online
 * node(s) over ESP-NOW. False when the gain is out of range or nobody could be reached. The C5
 * relearns its rests: the caller confirms the VTXs are off first. */
bool c6_hub_send_gain(uint8_t gate_index, uint8_t gain);
/* Passage calibration of a gate's C5 (link_proto CALIB_*, docs/detector.md): start a step (0 ends the
 * session, 1 passage, 2 away), read its latest state, apply the proposed thresholds. Gate 0 is the
 * local C5. False when the gate's C5 cannot be reached or there is nothing to apply. */
bool c6_hub_calib_start(uint8_t gate_index, uint8_t step, uint8_t channel);   /* channel: 0..7 or LP_CALIB_ANY (step 1) */
bool c6_hub_calib_get(uint8_t gate_index, lp_calib_t *out);
bool c6_hub_calib_apply(uint8_t gate_index);
/* A remote gate back on the fleet's thresholds: its calibration override is dropped and the fleet's
 * CONFIG sent. False for gate 0 (it keeps the C5's own settings) or a gate nobody answers for. */
bool c6_hub_calib_reset(uint8_t gate_index);
int32_t c6_hub_vtx_threshold_x10(void);        /* remote gates: C/N*10 above which a VTX is probably on (pushed active_db) */

/* Debug mode of a remote gate (1..): its C5's console text and keys, over the node (fleet_proto
 * DEBUG, KEY, LOG_REQ, LOG_CHUNK). One target at a time. on switches it on (the previous target
 * off) and false when the gate has no online node; off switches the target off. It leaves by
 * itself when the node reports it is out of debug (a reboot) or no c6_hub_log_read() comes for
 * 10 s. The log is a ring like the local C5's: positions count bytes since it was switched on. */
bool c6_hub_debug_set(uint8_t gate, bool on);
int  c6_hub_debug_gate(void);                  /* the target gate, -1 none */
size_t c6_hub_log_read(uint32_t since, char *out, size_t max, uint32_t *next, bool *lost);   /* renews the lease */
bool c6_hub_send_keys(uint8_t gate, const char *keys, size_t n);   /* KEY to the gate's node; false if not its target */
