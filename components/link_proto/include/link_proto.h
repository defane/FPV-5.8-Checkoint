/* link_proto: the C5 <-> bridge UART protocol for the checkpoint platform.
 *
 * The bridge (a C3 node, or the C6 master for its own gate) pushes detector
 * configuration and commands (calibrate, reboot) to the C5; the C5 reports
 * pass events and stats with its health back. In production nobody
 * opens the C5's USB console, so everything an operator needs goes through
 * here. Pure C, no hardware and no float on the wire: every value is a
 * fixed-point integer, little endian, so the two targets agree regardless of
 * FPU or struct padding.
 *
 * Frame: [SOF=0xC5][VER=4][TYPE][LEN][payload LEN bytes][CRC16_lo][CRC16_hi]
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
#define LP_VERSION  6u   /* 6: GAIN (the web sets the C5's RX gain), CALIB_START / CALIB_STATE (passage calibration). 5: LOG (C5 text), KEY (console keystrokes), DEBUG. 4: CONFIG carries the re-arm drop,
                          * STATS each channel's detector state. 3: the C5 scans: STATS levels, EVENT_PASS age */
#define LP_MAX_PAYLOAD 56u
#define LP_MAX_FRAME   (6u + LP_MAX_PAYLOAD)   /* SOF+VER+TYPE+LEN + payload + CRC16(2) */

typedef enum {
    LP_MSG_HELLO = 1,        /* either way: announce role */
    LP_MSG_CONFIG_SET = 2,   /* bridge -> C5: apply this configuration */
    LP_MSG_CONFIG_REQ = 3,   /* bridge -> C5: send me the current configuration */
    LP_MSG_CONFIG_STATE = 4, /* C5 -> bridge: current configuration (answer/echo) */
    LP_MSG_EVENT_PASS = 5,   /* C5 -> bridge: a drone passed on a channel */
    LP_MSG_STATS = 6,        /* C5 -> bridge: periodic levels, detector states, health */
    LP_MSG_COMMAND = 8,      /* bridge -> C5: run an lp_cmd_t */
    LP_MSG_LOG = 9,          /* C5 -> bridge: a chunk of the console's text output (debug mode) */
    LP_MSG_DEBUG = 11,       /* bridge -> C5: debug mode on/off (off by default: no LOG, KEY ignored) */
    LP_MSG_KEY = 10,         /* bridge -> C5: console keystrokes ("Ka", "T", "F" ...), debug mode */
    LP_MSG_GAIN = 12,        /* bridge -> C5: force this RX gain index (console K g/G, saved, rests relearnt) */
    LP_MSG_CALIB_START = 13, /* bridge -> C5: a passage-calibration step (1 passage, 2 away) or 0 to end it */
    LP_MSG_CALIB_STATE = 14, /* C5 -> bridge: the calibration session, every STATS period while one exists */
} lp_type_t;

typedef enum { LP_ROLE_C5 = 1, LP_ROLE_C3 = 2 } lp_role_t;

/* Passage calibration (components/pass_detector/pass_calib.h): with a drone in the passage, then 2 m
 * away, the C5 measures each channel's smoothed rise for LP_CALIB_SECS seconds and proposes thresholds.
 * It does not blind the gate, so the VTX stays ON. A session starts with CALIB_START step 1, goes on
 * with step 2 (and may redo either), and ends on step 0, on the bridge applying the proposal or
 * after LP_CALIB_IDLE_S s with nothing asked; CALIB_STATE is sent every STATS period meanwhile. */
#define LP_CALIB_SECS   5u
#define LP_CALIB_IDLE_S 600u
typedef enum {
    LP_CALIB_NONE = 0,       /* no session: never sent, the bridges' own idle */
    LP_CALIB_MEASURING1 = 1, /* the drone in the passage */
    LP_CALIB_MEASURED1 = 2,  /* ... measured, waiting for step 2 */
    LP_CALIB_MEASURING2 = 3, /* the drone 2 m away */
    LP_CALIB_DONE = 4,       /* both measured, thresholds proposed */
} lp_calib_phase_t;

typedef struct {
    uint8_t  phase;          /* lp_calib_phase_t */
    uint8_t  secs_left;      /* of the running measurement, else 0 */
    uint16_t flags;          /* PC_* of pass_calib.h (PC_BUSY: the last request was refused) */
    uint8_t  channel;        /* the drone's channel (R1 = 0), 255 before step 1 is measured */
    int8_t   gate[5];        /* in the passage: p10, p50, p90, p95, max of the channel's rise, half dB */
    uint8_t  gate_sat_pct;   /* captures that clipped in the passage, % */
    int8_t   away[5];        /* 2 m away: the same */
    int16_t  active_db_x10, hysteresis_db_x10, drop_db_x10, min_peak_db_x10;   /* the proposal (DONE only) */
    int8_t   gain_steps;     /* advice: -1 lower the gain by one step of 2, +1 raise it */
} lp_calib_t;

/* The 25-byte payload of an lp_calib_t, shared with fleet_proto's CALIB_STATE relay. */
#define LP_CALIB_LEN 25u
void lp_calib_pack(uint8_t out[LP_CALIB_LEN], const lp_calib_t *c);
bool lp_calib_unpack(const uint8_t *p, size_t len, lp_calib_t *c);   /* false: wrong length or phase */

/* The forced RX gain the web may set (the C5 clamps to its PHY's maximum; the console's K g/G
 * keeps its step of 2 from the saved one). Changing it blinds the gate for a moment: the rests are
 * learnt again, so every VTX must be off. */
#define LP_GAIN_MIN 10u
#define LP_GAIN_MAX 83u

/* COMMAND: what the console's B, F and a reset do, for a C5 nobody plugs USB
 * into. A calibration blinds the gate (no pass detection) while it runs and
 * while the rests are learnt again, about 3 s in all, and wants the VTXs off;
 * the caller confirms that (the C6/C3 web asks). */
typedef enum {
    LP_CMD_CAL_FULL  = 1,    /* console B: recalibrate every LO + verify image rejection (VTX off) */
    LP_CMD_CAL_QUICK = 2,    /* console F: recalibrate every LO (VTX off) */
    LP_CMD_REBOOT    = 3,    /* quiesce the engine, restart the C5 */
} lp_cmd_t;

/* Detector tunables, fixed point. Mirrors pass_detector's pd_config_t plus its
 * relative re-arm drop (pd_set_rearm); the C5 glue converts to/from them.
 * volume_pct/muted are kept for the wire layout only: the detector has no
 * sound and ignores them (echoes 0). Whoever builds one sets rearm_db_x10: 0
 * turns the relative re-arm off on the gate that takes it. */
typedef struct {
    int32_t  alpha_milli;       /* alpha * 1000, (0, 1000]: the weight per 17.5 ms reference
                                 * sweep (time constant tau = -17.5 ms / ln(1 - alpha)) */
    int32_t  active_db_x10;     /* active_db * 10 */
    int32_t  hysteresis_db_x10;
    int32_t  drop_db_x10;
    int32_t  min_peak_db_x10;
    uint32_t refractory_ms;
    uint32_t stale_ms;
    int32_t  rearm_db_x10;      /* the relative re-arm's drop * 10, 0..LP_REARM_MAX_X10; 0 = off
                                 * (the C5 refuses a CONFIG_SET outside that range) */
    uint8_t  volume_pct;        /* 0..100 */
    uint8_t  muted;             /* 0/1 */
} lp_config_t;

#define LP_REARM_MAX_X10     400   /* 40 dB, the top of CONFIG_CHECKPOINT_REARM_DROP_DB's range */
#define LP_REARM_DEFAULT_X10 160   /* 16 dB, its default: what the bridges' default configs carry */

typedef struct {
    uint8_t  channel;           /* 0..7 */
    int32_t  peak_db_x10;       /* rise above the channel's rest *10 */
    uint32_t at_ms;             /* the pass, on the C5's clock */
    uint16_t age_ms;            /* how long ago that was when it was sent */
} lp_event_t;

/* lp_stats_t.flags */
#define LP_HEALTH_CAL_NEEDED  0x01u  /* an LO's DC offset or IQ estimate out of range: run CAL_QUICK, VTX off
                                      * (also: no engine, or a stalled scan) */
#define LP_HEALTH_CALIBRATING 0x02u  /* a calibration, the rests' learning or a console bench: the gate is blind */

#define LP_LEVELS 8u                 /* R1..R8 */
#define LP_LEVEL_UNKNOWN (-128)      /* not measured: calibrating, a bench, no engine, no reading for 1 s
                                      * (its LO's captures fail, or the pair guard held it: its partner
                                      * clipped the ADC) */

#define LP_FW_LEN 24u                /* firmware id incl. NUL, e.g. "5.0.0-dev+3201b7196ece" */

/* lp_stats_t.states: each channel's pass_detector pd_state_t (0 IDLE, 1 RISING, 2 PEAK_PASSED,
 * 3 COOLDOWN), 2 bits per channel, R1 (channel 0) in bits 1..0, R8 in bits 15..14. PEAK_PASSED
 * lasts one reading, then COOLDOWN until the channel re-arms: both mean it waits to re-arm. */
#define LP_STATE_IDLE        0u
#define LP_STATE_RISING      1u
#define LP_STATE_PEAK_PASSED 2u
#define LP_STATE_COOLDOWN    3u

/* Channel ch's state in states; LP_STATE_IDLE for a channel past R8. */
static inline unsigned lp_state_of(uint16_t states, unsigned ch) {
    return ch < LP_LEVELS ? (states >> (2u * ch)) & 3u : LP_STATE_IDLE;
}

/* states with channel ch's state set to state (its low 2 bits); unchanged for a channel past R8. */
static inline uint16_t lp_state_set(uint16_t states, unsigned ch, unsigned state) {
    if (ch >= LP_LEVELS) return states;
    const unsigned at = 2u * ch;
    return (uint16_t)((states & ~(3u << at)) | ((state & 3u) << at));
}

/* The C5's live state and health, every CONFIG_CHECKPOINT_LINK_STATS_MS: what
 * the console's T would show an operator, reduced to what a web page needs. */
typedef struct {
    int32_t  cn_x10;            /* the strongest channel's rise *10; LP_CN_UNKNOWN when none
                                 * (the bridges' VTX guard for a calibration) */
    uint16_t states;            /* each channel's detector state, lp_state_of(); 0 (all IDLE)
                                 * when the gate does not scan */
    uint8_t  flags;             /* LP_HEALTH_* */
    uint8_t  gain;              /* forced RX gain index */
    uint32_t passes;            /* passes detected since boot */
    uint32_t uptime_s;
    uint32_t heap_min;          /* lowest free internal heap since boot, bytes */
    uint32_t faults;            /* capture faults (timeouts, short/overrun dumps, DSP errors) + scan stalls */
    int8_t   levels[LP_LEVELS]; /* dB above each channel's rest, LP_LEVEL_UNKNOWN when none */
    char     fw[LP_FW_LEN];     /* firmware id; always NUL-terminated after a decode */
} lp_stats_t;

#define LP_CN_UNKNOWN (-990)

/* LOG and KEY carry raw bytes: a LOG chunk is any slice of the console text (not line aligned),
 * a KEY frame is typed on the console in order (the C5 drops the download key). */
#define LP_TEXT_MAX LP_MAX_PAYLOAD
#define LP_KEY_MAX  16u
typedef struct {
    uint8_t len;                /* 1..LP_TEXT_MAX (LOG) or 1..LP_KEY_MAX (KEY) */
    char    text[LP_TEXT_MAX];
} lp_text_t;

typedef struct {
    lp_type_t type;
    union {
        lp_config_t config;     /* CONFIG_SET / CONFIG_STATE */
        lp_event_t  event;      /* EVENT_PASS */
        lp_stats_t  stats;      /* STATS */
        uint8_t     role;       /* HELLO */
        uint8_t     command;    /* COMMAND: an lp_cmd_t (unknown values are not decoded) */
        lp_text_t   data;       /* LOG / KEY */
        uint8_t     debug;      /* DEBUG: 0 off, 1 on */
        uint8_t     gain;       /* GAIN: LP_GAIN_MIN..LP_GAIN_MAX */
        uint8_t     calib_step; /* CALIB_START: 0 end, 1 passage, 2 away */
        lp_calib_t  calib;      /* CALIB_STATE */
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
/* 0 when len is 0 or over LP_TEXT_MAX / LP_KEY_MAX. */
size_t lp_encode_log(uint8_t *buf, size_t cap, const char *text, size_t len);
size_t lp_encode_debug(uint8_t *buf, size_t cap, bool on);
size_t lp_encode_calib_start(uint8_t *buf, size_t cap, uint8_t step);   /* 0 over 2 */
size_t lp_encode_calib_state(uint8_t *buf, size_t cap, const lp_calib_t *c);   /* 0 for a phase over DONE */
size_t lp_encode_gain(uint8_t *buf, size_t cap, uint8_t gain);   /* 0 outside LP_GAIN_MIN..LP_GAIN_MAX */
size_t lp_encode_key(uint8_t *buf, size_t cap, const char *keys, size_t len);

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

/* ---- debug mode, bridge side (the C3 and the C6 share it) ----
 * The C5's console text, kept by the bridge for its web page. A ring of `cap` bytes the caller
 * provides; positions count bytes since the last reset. No locking: the caller serialises. */
typedef struct {
    char    *buf;
    uint32_t cap;
    uint32_t total;             /* bytes ever stored: the position of the next one */
} lp_logring_t;

void lp_logring_init(lp_logring_t *r, char *buf, uint32_t cap);
void lp_logring_reset(lp_logring_t *r);                  /* a fresh log: positions restart at 0 */
/* Stores a LOG chunk made safe for JSON: no CR, no quote (-> '), no backslash (-> /), no control
 * other than the newline, nothing over '~' (-> '?'). */
void lp_logring_put(lp_logring_t *r, const lp_text_t *t);
/* Copies up to max bytes from position `since`. *next is where to continue; *lost is set when the
 * ring had already dropped bytes after `since` (they are skipped). `since` past the end (the
 * bridge rebooted since the page asked) restarts at the oldest byte kept. */
size_t lp_logring_read(const lp_logring_t *r, uint32_t since, char *out, size_t max,
                       uint32_t *next, bool *lost);

/* Keystrokes a page may type on the C5's console: 1..LP_KEY_MAX printable ASCII, no backtick (the
 * C5's download key). */
bool lp_keys_valid(const char *keys);
/* Would these keystrokes blind the gate (no detection for a few seconds or the length of a bench)?
 * F and B recalibrate; after K, the benches (s z x n p S), the gain (g G) and the antenna (i) do.
 * K stays open until q (the C5's own window is 5 s per key, and a request's keys arrive together). */
bool lp_keys_blind(const char *keys);

/* CRC16-CCITT over len bytes (exposed for tests). */
uint16_t lp_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
