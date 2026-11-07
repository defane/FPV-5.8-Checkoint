/* link_proto: the C5 <-> bridge UART protocol for the checkpoint platform.
 *
 * The bridge (a C3 node, or the C6 master for its own gate) pushes detector
 * configuration, a channel and commands (calibrate, reboot) to the C5; the C5
 * reports pass events and stats with its health back. In production nobody
 * opens the C5's USB console, so everything an operator needs goes through
 * here. Pure C, no hardware and no float on the wire: every value is a
 * fixed-point integer, little endian, so the two targets agree regardless of
 * FPU or struct padding.
 *
 * Frame: [SOF=0xC5][VER=2][TYPE][LEN][payload LEN bytes][CRC16_lo][CRC16_hi]
 * CRC16 is CCITT (poly 0x1021, init 0xFFFF) over VER,TYPE,LEN,payload. A byte
 * other than SOF where SOF is expected is dropped, so the stream resynchronises
 * after noise. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LP_SOF      0xC5u
#define LP_VERSION  3u   /* 3: the C5 scans: STATS levels, EVENT_PASS age, no SET_CHANNEL */
#define LP_MAX_PAYLOAD 56u
#define LP_MAX_FRAME   (6u + LP_MAX_PAYLOAD)   /* SOF+VER+TYPE+LEN + payload + CRC16(2) */

typedef enum {
    LP_MSG_HELLO = 1,        /* either way: announce role */
    LP_MSG_CONFIG_SET = 2,   /* bridge -> C5: apply this configuration */
    LP_MSG_CONFIG_REQ = 3,   /* bridge -> C5: send me the current configuration */
    LP_MSG_CONFIG_STATE = 4, /* C5 -> bridge: current configuration (answer/echo) */
    LP_MSG_EVENT_PASS = 5,   /* C5 -> bridge: a drone passed on a channel */
    LP_MSG_STATS = 6,        /* C5 -> bridge: periodic live C/N, detector state, health */
    LP_MSG_COMMAND = 8,      /* bridge -> C5: run an lp_cmd_t */
} lp_type_t;

typedef enum { LP_ROLE_C5 = 1, LP_ROLE_C3 = 2 } lp_role_t;

/* COMMAND: what the console's B, F and a reset do, for a C5 nobody plugs USB
 * into. A calibration blinds the gate while it runs (no pass detection) and
 * wants the VTXs off; the caller confirms that (the C6/C3 web asks). */
typedef enum {
    LP_CMD_CAL_FULL  = 1,    /* console B: full RF optimisation (ground only, VTX off) */
    LP_CMD_CAL_QUICK = 2,    /* console F: calibrate the current filter */
    LP_CMD_REBOOT    = 3,    /* restart the C5 */
} lp_cmd_t;

/* Detector tunables, fixed point. Mirrors pass_detector's pd_config_t; the C5
 * glue converts to/from pd_config_t. volume_pct/muted are kept for the wire
 * layout only: the detector has no sound and ignores them (echoes 0). */
typedef struct {
    int32_t  alpha_milli;       /* alpha * 1000, (0, 1000] */
    int32_t  active_db_x10;     /* active_db * 10 */
    int32_t  hysteresis_db_x10;
    int32_t  drop_db_x10;
    int32_t  min_peak_db_x10;
    uint32_t refractory_ms;
    uint32_t stale_ms;
    uint8_t  volume_pct;        /* 0..100 */
    uint8_t  muted;             /* 0/1 */
} lp_config_t;

typedef struct {
    uint8_t  channel;           /* 0..7 */
    int32_t  peak_db_x10;       /* rise above the channel's rest *10 */
    uint32_t at_ms;             /* the pass, on the C5's clock */
    uint16_t age_ms;            /* how long ago that was when it was sent */
} lp_event_t;

/* lp_stats_t.flags */
#define LP_HEALTH_CAL_NEEDED  0x01u  /* the DC servo cannot hold a row: run CAL_FULL, VTX off */
#define LP_HEALTH_CALIBRATING 0x02u  /* a calibration (boot, F or B) owns the receiver */

#define LP_LEVELS 8u                 /* R1..R8 */
#define LP_LEVEL_UNKNOWN (-128)      /* not measured: calibrating, no scan */

#define LP_FW_LEN 24u                /* firmware id incl. NUL, e.g. "5.0.0-dev+3201b7196ece" */

/* The C5's live state and health, every CONFIG_CHECKPOINT_LINK_STATS_MS: what
 * the console's T would show an operator, reduced to what a web page needs. */
typedef struct {
    int32_t  cn_x10;            /* the strongest channel's level *10; LP_CN_UNKNOWN when none */
    uint8_t  channel;           /* that channel, 0..7 */
    uint8_t  state;             /* its pd_state_t */
    uint8_t  flags;             /* LP_HEALTH_* */
    uint8_t  gain;              /* current RX gain index */
    uint32_t passes;            /* passes detected since boot */
    uint32_t uptime_s;
    uint32_t heap_min;          /* lowest free internal heap since boot, bytes */
    uint32_t faults;            /* transport fault counters (TX empty, RX overflow, DMA, BitScrambler) */
    int8_t   levels[LP_LEVELS]; /* dB above each channel's rest, LP_LEVEL_UNKNOWN when none */
    char     fw[LP_FW_LEN];     /* firmware id; always NUL-terminated after a decode */
} lp_stats_t;

#define LP_CN_UNKNOWN (-990)

typedef struct {
    lp_type_t type;
    union {
        lp_config_t config;     /* CONFIG_SET / CONFIG_STATE */
        lp_event_t  event;      /* EVENT_PASS */
        lp_stats_t  stats;      /* STATS */
        uint8_t     role;       /* HELLO */
        uint8_t     command;    /* COMMAND: an lp_cmd_t (unknown values are not decoded) */
    };
} lp_msg_t;

/* Encoders: write a full frame into buf[cap]; return its length, or 0 if it
 * would not fit. */
size_t lp_encode_hello(uint8_t *buf, size_t cap, lp_role_t role);
size_t lp_encode_config(uint8_t *buf, size_t cap, lp_type_t type, const lp_config_t *c);
size_t lp_encode_config_req(uint8_t *buf, size_t cap);
size_t lp_encode_event(uint8_t *buf, size_t cap, const lp_event_t *e);
size_t lp_encode_stats(uint8_t *buf, size_t cap, const lp_stats_t *s);
size_t lp_encode_command(uint8_t *buf, size_t cap, lp_cmd_t cmd);   /* 0 for an unknown cmd */

/* Streaming decoder: feed one received byte at a time. Returns true and fills
 * *out when a complete, CRC-valid frame ends on this byte. It buffers at most
 * one frame and rescans on a bad SOF, version, length or CRC, so the stream
 * recovers from noise or a truncated frame without swallowing the next one. */
typedef struct {
    uint8_t buf[LP_MAX_FRAME];
    uint8_t len;                /* bytes currently buffered */
} lp_decoder_t;

void lp_decoder_init(lp_decoder_t *d);
bool lp_decoder_push(lp_decoder_t *d, uint8_t byte, lp_msg_t *out);

/* CRC16-CCITT over len bytes (exposed for tests). */
uint16_t lp_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
