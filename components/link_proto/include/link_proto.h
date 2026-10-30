/* link_proto: the C5 <-> C3 UART protocol for the checkpoint platform.
 *
 * The C3 (config/telemetry bridge, WiFi + web) pushes detector configuration
 * to the C5 and the C5 reports pass events and stats back. Pure C, no hardware
 * and no float on the wire: every value is a fixed-point integer, little
 * endian, so the two targets agree regardless of FPU or struct padding.
 *
 * Frame: [SOF=0xC5][VER=1][TYPE][LEN][payload LEN bytes][CRC16_lo][CRC16_hi]
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
#define LP_VERSION  1u
#define LP_MAX_PAYLOAD 48u
#define LP_MAX_FRAME   (6u + LP_MAX_PAYLOAD)   /* SOF+VER+TYPE+LEN + payload + CRC16(2) */

typedef enum {
    LP_MSG_HELLO = 1,        /* either way: announce role */
    LP_MSG_CONFIG_SET = 2,   /* C3 -> C5: apply this configuration */
    LP_MSG_CONFIG_REQ = 3,   /* C3 -> C5: send me the current configuration */
    LP_MSG_CONFIG_STATE = 4, /* C5 -> C3: current configuration (answer/echo) */
    LP_MSG_EVENT_PASS = 5,   /* C5 -> C3: a drone passed on a channel */
    LP_MSG_STATS = 6,        /* C5 -> C3: periodic counters + live C/N */
} lp_type_t;

typedef enum { LP_ROLE_C5 = 1, LP_ROLE_C3 = 2 } lp_role_t;

/* Detector + audio tunables, fixed point. Mirrors pass_detector's pd_config_t
 * plus audio volume/mute; the C5 glue converts to/from pd_config_t. */
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
    int32_t  peak_db_x10;
    uint32_t at_ms;
} lp_event_t;

typedef struct {
    uint32_t played, dropped, muted, failed;
    int32_t  cn_x10;            /* current C/N *10; LP_CN_UNKNOWN when none */
    uint8_t  channel;           /* 0..7 */
    uint8_t  state;             /* pd_state_t */
} lp_stats_t;

#define LP_CN_UNKNOWN (-990)

typedef struct {
    lp_type_t type;
    union {
        lp_config_t config;     /* CONFIG_SET / CONFIG_STATE */
        lp_event_t  event;      /* EVENT_PASS */
        lp_stats_t  stats;      /* STATS */
        uint8_t     role;       /* HELLO */
    };
} lp_msg_t;

/* Encoders: write a full frame into buf[cap]; return its length, or 0 if it
 * would not fit. */
size_t lp_encode_hello(uint8_t *buf, size_t cap, lp_role_t role);
size_t lp_encode_config(uint8_t *buf, size_t cap, lp_type_t type, const lp_config_t *c);
size_t lp_encode_config_req(uint8_t *buf, size_t cap);
size_t lp_encode_event(uint8_t *buf, size_t cap, const lp_event_t *e);
size_t lp_encode_stats(uint8_t *buf, size_t cap, const lp_stats_t *s);

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
