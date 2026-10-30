/* fleet_proto: the C6-master <-> C3-node ESP-NOW protocol for the race-timing
 * fleet. ESP-NOW delivers discrete datagrams (its own length + FCS), so this is
 * a typed-message (de)serialisation, not a byte stream: one frame per message,
 * integer-only little-endian payloads, guarded by a version byte and a CRC16 so
 * a mismatched firmware or a stray frame is rejected.
 *
 * Frame: [VER=1][TYPE][payload...][CRC16_lo][CRC16_hi], CRC16-CCITT over
 * VER,TYPE,payload (reuses link_proto's lp_crc16). Detector/audio tunables ride
 * as link_proto's lp_config_t so the C6 forwards them to a C5 over UART
 * unchanged.
 *
 * Roles: the C6 is the master and the fleet time source; a C3 (or the master's
 * own set) is a node = one gate. A channel code identifies the FPV channel a
 * gate watches (one pilot). */
#pragma once
#include "link_proto.h"   /* lp_config_t, lp_crc16 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FP_VERSION      1u
#define FP_MAX_PAYLOAD  64u
#define FP_MAX_FRAME    (4u + FP_MAX_PAYLOAD)   /* VER+TYPE + payload + CRC16(2) */

typedef enum {
    FP_MSG_PAIR_REQUEST = 1,  /* node  -> master: announce for pairing */
    FP_MSG_PAIR_ACCEPT  = 2,  /* master -> node:  accepted, assign id/gate/channel */
    FP_MSG_TIME_BEACON  = 3,  /* master -> all:   fleet time for offset estimation */
    FP_MSG_CONFIG       = 4,  /* master -> node:  gate/channel/enable + detector cfg */
    FP_MSG_GATE_PASS    = 5,  /* node  -> master: a pass at this gate (fleet-timed) */
    FP_MSG_NODE_STATUS  = 6,  /* node  -> master: periodic C/N, state, counters */
    FP_MSG_HEARTBEAT    = 7,  /* either:          keepalive */
} fp_type_t;

typedef enum { FP_ROLE_NODE = 1, FP_ROLE_MASTER = 2 } fp_role_t;

typedef struct {
    uint32_t nonce;           /* node-chosen, echoed in PAIR_ACCEPT */
    uint8_t  seen_channel;    /* channel the node's C5 is seeing now (hint) */
    uint8_t  fw_major;
} fp_pair_request_t;

typedef struct {
    uint32_t nonce;           /* echoes the request */
    uint8_t  node_id;
    uint8_t  gate_index;      /* position along the track, 0-based */
    uint8_t  channel;         /* assigned FPV channel code */
} fp_pair_accept_t;

typedef struct {
    uint32_t seq;
    uint32_t fleet_ms;        /* master clock when the beacon was sent */
} fp_time_beacon_t;

typedef struct {
    uint8_t     gate_index;
    uint8_t     channel;
    uint8_t     enable;       /* 0: node idle; 1: detect+report */
    lp_config_t det;          /* forwarded to the C5 over link_proto */
} fp_config_t;

typedef struct {
    uint8_t  gate_index;
    uint8_t  channel;
    int32_t  peak_db_x10;
    uint32_t fleet_ms;        /* pass time in fleet time (node-estimated) */
    uint32_t seq;             /* per-node pass counter (dedup across retries) */
} fp_gate_pass_t;

typedef struct {
    uint8_t  gate_index;
    uint8_t  channel;
    int32_t  cn_x10;          /* LP_CN_UNKNOWN when none */
    uint8_t  state;           /* pd_state_t */
    uint32_t passes;
    uint8_t  flags;
} fp_node_status_t;

typedef struct {
    uint8_t  role;            /* fp_role_t */
    uint32_t uptime_ms;
} fp_heartbeat_t;

typedef struct {
    fp_type_t type;
    union {
        fp_pair_request_t pair_request;
        fp_pair_accept_t  pair_accept;
        fp_time_beacon_t  time_beacon;
        fp_config_t       config;
        fp_gate_pass_t    gate_pass;
        fp_node_status_t  node_status;
        fp_heartbeat_t    heartbeat;
    };
} fp_msg_t;

/* Encoders: write a full frame into buf[cap]; return its length, or 0 if it
 * would not fit. */
size_t fp_encode_pair_request(uint8_t *buf, size_t cap, const fp_pair_request_t *m);
size_t fp_encode_pair_accept(uint8_t *buf, size_t cap, const fp_pair_accept_t *m);
size_t fp_encode_time_beacon(uint8_t *buf, size_t cap, const fp_time_beacon_t *m);
size_t fp_encode_config(uint8_t *buf, size_t cap, const fp_config_t *m);
size_t fp_encode_gate_pass(uint8_t *buf, size_t cap, const fp_gate_pass_t *m);
size_t fp_encode_node_status(uint8_t *buf, size_t cap, const fp_node_status_t *m);
size_t fp_encode_heartbeat(uint8_t *buf, size_t cap, const fp_heartbeat_t *m);

/* Decode one received datagram. True and fills *out on a valid frame. */
bool fp_decode(const uint8_t *buf, size_t n, fp_msg_t *out);

#ifdef __cplusplus
}
#endif
