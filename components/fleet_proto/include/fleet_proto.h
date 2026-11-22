/* fleet_proto: the C6-master <-> C3-node ESP-NOW protocol for the race-timing
 * fleet. ESP-NOW delivers discrete datagrams (its own length + FCS), so this is
 * a typed-message (de)serialisation, not a byte stream: one frame per message,
 * integer-only little-endian payloads, guarded by a version byte and a CRC16 so
 * a mismatched firmware or a stray frame is rejected.
 *
 * Frame: [VER=6][TYPE][payload...][CRC16_lo][CRC16_hi], CRC16-CCITT over
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

#define FP_VERSION      6u   /* 6: GAIN (the master sets a gate's RX gain);
                              * 5: DEBUG, KEY, LOG_REQ, LOG_CHUNK (a remote gate's console on the master's page);
                              * 4: CONFIG's lp_config_t and NODE_STATUS carry the re-arm drop,
                              * NODE_STATUS each channel's detector state;
                              * 3: NODE_STATUS levels, the C5 scans; 2: seq + health, COMMAND */
#define FP_MAX_PAYLOAD  64u
#define FP_MAX_FRAME    (4u + FP_MAX_PAYLOAD)   /* VER+TYPE + payload + CRC16(2) */

typedef enum {
    FP_MSG_PAIR_REQUEST = 1,  /* node  -> master: announce for pairing */
    FP_MSG_PAIR_ACCEPT  = 2,  /* master -> node:  accepted, assign id/gate/channel */
    FP_MSG_TIME_BEACON  = 3,  /* master -> all:   fleet time for offset estimation */
    FP_MSG_CONFIG       = 4,  /* master -> node:  gate/channel/enable + detector cfg */
    FP_MSG_GATE_PASS    = 5,  /* node  -> master: a pass at this gate (fleet-timed) */
    FP_MSG_NODE_STATUS  = 6,  /* node  -> master: periodic C/N, states, counters, C5 health */
    FP_MSG_HEARTBEAT    = 7,  /* node  -> master: boot probe, until it hears us; answered with CONFIG */
    FP_MSG_COMMAND      = 8,  /* master -> node:  forward an lp_cmd_t to the node's C5 */
    FP_MSG_DEBUG        = 9,  /* master -> node:  debug mode (the C5's console log + keys) on or off */
    FP_MSG_KEY          = 10, /* master -> node:  keys to type on the node's C5 console (debug on) */
    FP_MSG_LOG_REQ      = 11, /* master -> node:  the console text after a position (stop-and-wait) */
    FP_MSG_LOG_CHUNK    = 12, /* node  -> master: up to LP_TEXT_MAX bytes of it, and the node's debug state */
    FP_MSG_GAIN         = 13, /* master -> node:  force the node's C5 RX gain (link_proto GAIN) */
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

/* The node's C5 STATS (link_proto lp_stats_t) relayed, plus the fleet's own
 * gate/seq and the re-arm drop its C5 echoed. Its 64-byte payload fills FP_MAX_PAYLOAD. */
typedef struct {
    uint8_t  gate_index;
    uint8_t  channel;         /* the node's assigned channel code */
    int32_t  cn_x10;          /* the C5's strongest rise *10, LP_CN_UNKNOWN when none */
    uint16_t states;          /* each channel's detector state as lp_stats_t carries it (lp_state_of) */
    uint32_t passes;          /* passes the C5 detected since its boot */
    uint8_t  flags;           /* LP_HEALTH_* of the C5 */
    uint32_t seq;             /* per-node status counter; master infers loss from gaps */
    uint8_t  gain;            /* C5 RX gain index */
    uint32_t uptime_s;        /* C5 uptime */
    uint32_t heap_min;        /* C5 lowest free internal heap since boot, bytes */
    uint32_t faults;          /* C5 capture faults (timeouts, short/overrun dumps, DSP errors)
                               * + scan stalls, since its boot */
    int8_t   levels[LP_LEVELS];   /* C5 dB above each channel's rest, LP_LEVEL_UNKNOWN when none */
    uint16_t rearm_db_x10;    /* the C5's re-arm drop *10 from its last CONFIG_STATE echo,
                               * FP_REARM_UNKNOWN before the first one */
    char     fw[LP_FW_LEN];   /* C5 firmware id; always NUL-terminated after a decode */
} fp_node_status_t;

#define FP_REARM_UNKNOWN 0xFFFFu

typedef struct {
    uint8_t  gate_index;      /* the gate it is meant for (a node ignores another's) */
    uint8_t  cmd;             /* lp_cmd_t */
} fp_command_t;

/* Debug mode of one remote gate, pulled by the master (a push would load the air for every gate):
 * the node's C3 keeps its C5's console text in a ring (link_proto's LOG frames) and answers each
 * LOG_REQ with the next bytes. All four count only from the node's own master (its MAC). The node
 * leaves debug mode itself when no LOG_REQ came for FP_DEBUG_LEASE_MS (a master that rebooted or
 * lost its DEBUG off). */
#define FP_DEBUG_LEASE_MS 30000u
#define FP_LOG_DEBUG      1u   /* LOG_CHUNK flag: the node's debug mode is on (off after its reboot) */
#define FP_LOG_LOST       2u   /* LOG_CHUNK flag: the ring had dropped bytes after the position asked */
typedef struct { uint8_t on; } fp_debug_t;
typedef struct { uint32_t since; } fp_log_req_t;
typedef struct {
    uint32_t from;            /* position (bytes since debug was switched on) of text[0]: the master drops
                               * what it already has and marks a gap */
    uint8_t  flags;           /* FP_LOG_* */
    uint8_t  len;             /* 0..LP_TEXT_MAX */
    char     text[LP_TEXT_MAX];   /* sanitized by the node (lp_logring) */
} fp_log_chunk_t;

typedef struct {
    uint8_t  gate_index;      /* the gate it is meant for (a node answers to its master's MAC, as COMMAND) */
    uint8_t  gain;            /* LP_GAIN_MIN..LP_GAIN_MAX */
} fp_gain_t;

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
        fp_command_t      command;
        fp_debug_t        debug;
        lp_text_t         key;        /* 1..LP_KEY_MAX bytes */
        fp_log_req_t      log_req;
        fp_log_chunk_t    log_chunk;
        fp_gain_t         gain;
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
size_t fp_encode_command(uint8_t *buf, size_t cap, const fp_command_t *m);   /* 0 for an unknown cmd */
size_t fp_encode_gain(uint8_t *buf, size_t cap, const fp_gain_t *m);   /* 0 outside LP_GAIN_MIN..LP_GAIN_MAX */
size_t fp_encode_debug(uint8_t *buf, size_t cap, bool on);
size_t fp_encode_key(uint8_t *buf, size_t cap, const char *keys, size_t len);   /* 0 unless 1..LP_KEY_MAX */
size_t fp_encode_log_req(uint8_t *buf, size_t cap, uint32_t since);
size_t fp_encode_log_chunk(uint8_t *buf, size_t cap, const fp_log_chunk_t *m);   /* 0 over LP_TEXT_MAX */

/* The master's side of the pull: what to keep of a LOG_CHUNK. *expected is the position of the next
 * byte it has not got; the chunk's text starts at c->from. A repeat (a request asked twice) gives
 * nothing, one that overlaps what we have gives its tail (*skip bytes of head to drop), and one
 * that starts later means bytes were lost (*gap of them, the node's ring or the air). Returns true
 * when there is text to keep, and moves *expected past it. */
bool fp_log_take(uint32_t *expected, const fp_log_chunk_t *c, uint8_t *skip, uint32_t *gap);

/* Decode one received datagram. True and fills *out on a valid frame. */
bool fp_decode(const uint8_t *buf, size_t n, fp_msg_t *out);

#ifdef __cplusplus
}
#endif
