/* link_proto: C5 <-> C3 UART framing and messages. See link_proto.h. */
#include "link_proto.h"

/* ---- little-endian field helpers ---- */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_i32(uint8_t *p, int32_t v) { put_u32(p, (uint32_t)v); }
static int32_t get_i32(const uint8_t *p) { return (int32_t)get_u32(p); }

/* ---- CRC16-CCITT (poly 0x1021, init 0xFFFF) ---- */

static uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    crc ^= (uint16_t)byte << 8;
    for (int b = 0; b < 8; b++)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                              : (uint16_t)(crc << 1);
    return crc;
}

uint16_t lp_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) crc = crc16_update(crc, data[i]);
    return crc;
}

/* ---- payload (de)serialisation ---- */

#define LP_LEN_HELLO   1u
#define LP_LEN_REQ     0u
#define LP_LEN_CONFIG  30u
#define LP_LEN_EVENT   11u
#define LP_LEN_STATS   (32u + LP_FW_LEN)   /* = LP_MAX_PAYLOAD */
#define LP_LEN_CMD     1u

static bool cmd_known(uint8_t c) {
    return c == LP_CMD_CAL_FULL || c == LP_CMD_CAL_QUICK || c == LP_CMD_REBOOT;
}

static void put_config(uint8_t *p, const lp_config_t *c) {
    put_i32(p + 0,  c->alpha_milli);
    put_i32(p + 4,  c->active_db_x10);
    put_i32(p + 8,  c->hysteresis_db_x10);
    put_i32(p + 12, c->drop_db_x10);
    put_i32(p + 16, c->min_peak_db_x10);
    put_u32(p + 20, c->refractory_ms);
    put_u32(p + 24, c->stale_ms);
    p[28] = c->volume_pct;
    p[29] = c->muted;
}

static void get_config(const uint8_t *p, lp_config_t *c) {
    c->alpha_milli       = get_i32(p + 0);
    c->active_db_x10     = get_i32(p + 4);
    c->hysteresis_db_x10 = get_i32(p + 8);
    c->drop_db_x10       = get_i32(p + 12);
    c->min_peak_db_x10   = get_i32(p + 16);
    c->refractory_ms     = get_u32(p + 20);
    c->stale_ms          = get_u32(p + 24);
    c->volume_pct        = p[28];
    c->muted             = p[29];
}

/* ---- encoders ---- */

static size_t frame(uint8_t *buf, size_t cap, uint8_t type,
                    const uint8_t *payload, uint8_t len) {
    const size_t total = 6u + (size_t)len;
    if (cap < total) return 0;
    buf[0] = LP_SOF;
    buf[1] = LP_VERSION;
    buf[2] = type;
    buf[3] = len;
    for (uint8_t i = 0; i < len; i++) buf[4 + i] = payload[i];
    const uint16_t crc = lp_crc16(&buf[1], (size_t)3 + len); /* VER,TYPE,LEN,payload */
    buf[4 + len] = (uint8_t)(crc & 0xFFu);
    buf[5 + len] = (uint8_t)(crc >> 8);
    return total;
}

size_t lp_encode_hello(uint8_t *buf, size_t cap, lp_role_t role) {
    const uint8_t pl = (uint8_t)role;
    return frame(buf, cap, LP_MSG_HELLO, &pl, LP_LEN_HELLO);
}

size_t lp_encode_config(uint8_t *buf, size_t cap, lp_type_t type,
                        const lp_config_t *c) {
    if (type != LP_MSG_CONFIG_SET && type != LP_MSG_CONFIG_STATE) return 0;
    uint8_t pl[LP_LEN_CONFIG];
    put_config(pl, c);
    return frame(buf, cap, (uint8_t)type, pl, LP_LEN_CONFIG);
}

size_t lp_encode_config_req(uint8_t *buf, size_t cap) {
    return frame(buf, cap, LP_MSG_CONFIG_REQ, NULL, LP_LEN_REQ);
}

size_t lp_encode_event(uint8_t *buf, size_t cap, const lp_event_t *e) {
    uint8_t pl[LP_LEN_EVENT];
    pl[0] = e->channel;
    put_i32(pl + 1, e->peak_db_x10);
    put_u32(pl + 5, e->at_ms);
    pl[9] = (uint8_t)(e->age_ms & 0xFFu);
    pl[10] = (uint8_t)(e->age_ms >> 8);
    return frame(buf, cap, LP_MSG_EVENT_PASS, pl, LP_LEN_EVENT);
}

size_t lp_encode_stats(uint8_t *buf, size_t cap, const lp_stats_t *s) {
    uint8_t pl[LP_LEN_STATS];
    put_i32(pl + 0,  s->cn_x10);
    pl[4] = s->channel;
    pl[5] = s->state;
    pl[6] = s->flags;
    pl[7] = s->gain;
    put_u32(pl + 8,  s->passes);
    put_u32(pl + 12, s->uptime_s);
    put_u32(pl + 16, s->heap_min);
    put_u32(pl + 20, s->faults);
    for (unsigned k = 0; k < LP_LEVELS; k++) pl[24 + k] = (uint8_t)s->levels[k];
    /* NUL-padded, never unterminated: the last byte is always 0. */
    unsigned i = 0;
    for (; i + 1u < LP_FW_LEN && s->fw[i]; i++) pl[32 + i] = (uint8_t)s->fw[i];
    for (; i < LP_FW_LEN; i++) pl[32 + i] = 0;
    return frame(buf, cap, LP_MSG_STATS, pl, LP_LEN_STATS);
}

size_t lp_encode_command(uint8_t *buf, size_t cap, lp_cmd_t cmd) {
    const uint8_t pl = (uint8_t)cmd;
    if (!cmd_known(pl)) return 0;
    return frame(buf, cap, LP_MSG_COMMAND, &pl, LP_LEN_CMD);
}

/* ---- streaming decoder ----
 *
 * Bytes accumulate in d->buf; after each byte we try to parse a frame anchored
 * at buf[0]. A byte that cannot begin a valid frame (not SOF, wrong version or
 * length) is dropped from the front and parsing retries, so a truncated frame
 * or line noise never consumes the frame that follows. */

void lp_decoder_init(lp_decoder_t *d) { d->len = 0; }

static void drop_front(lp_decoder_t *d, uint8_t n) {
    if (n >= d->len) { d->len = 0; return; }
    for (uint8_t i = 0; i + n < d->len; i++) d->buf[i] = d->buf[i + n];
    d->len = (uint8_t)(d->len - n);
}

static bool decode_payload(uint8_t type, uint8_t len, const uint8_t *p,
                           lp_msg_t *out) {
    switch (type) {
    case LP_MSG_HELLO:
        if (len != LP_LEN_HELLO) return false;
        out->type = LP_MSG_HELLO;
        out->role = p[0];
        return true;
    case LP_MSG_CONFIG_REQ:
        if (len != LP_LEN_REQ) return false;
        out->type = LP_MSG_CONFIG_REQ;
        return true;
    case LP_MSG_CONFIG_SET:
    case LP_MSG_CONFIG_STATE:
        if (len != LP_LEN_CONFIG) return false;
        out->type = (lp_type_t)type;
        get_config(p, &out->config);
        return true;
    case LP_MSG_EVENT_PASS:
        if (len != LP_LEN_EVENT) return false;
        out->type = LP_MSG_EVENT_PASS;
        out->event.channel = p[0];
        out->event.peak_db_x10 = get_i32(p + 1);
        out->event.at_ms = get_u32(p + 5);
        out->event.age_ms = (uint16_t)(p[9] | ((uint16_t)p[10] << 8));
        return true;
    case LP_MSG_STATS:
        if (len != LP_LEN_STATS) return false;
        out->type = LP_MSG_STATS;
        out->stats.cn_x10   = get_i32(p + 0);
        out->stats.channel  = p[4];
        out->stats.state    = p[5];
        out->stats.flags    = p[6];
        out->stats.gain     = p[7];
        out->stats.passes   = get_u32(p + 8);
        out->stats.uptime_s = get_u32(p + 12);
        out->stats.heap_min = get_u32(p + 16);
        out->stats.faults   = get_u32(p + 20);
        for (unsigned k = 0; k < LP_LEVELS; k++) out->stats.levels[k] = (int8_t)p[24 + k];
        for (unsigned i = 0; i < LP_FW_LEN; i++) out->stats.fw[i] = (char)p[32 + i];
        out->stats.fw[LP_FW_LEN - 1u] = '\0';   /* a peer may not have terminated it */
        return true;
    case LP_MSG_COMMAND:
        if (len != LP_LEN_CMD || !cmd_known(p[0])) return false;
        out->type = LP_MSG_COMMAND;
        out->command = p[0];
        return true;
    default:
        return false; /* unknown type: drop */
    }
}

bool lp_decoder_push(lp_decoder_t *d, uint8_t byte, lp_msg_t *out) {
    if (d->len >= LP_MAX_FRAME) drop_front(d, 1); /* make room; never a full valid frame */
    d->buf[d->len++] = byte;

    for (;;) {
        if (d->len == 0) return false;
        if (d->buf[0] != LP_SOF) { drop_front(d, 1); continue; }
        if (d->len < 4) return false;              /* need SOF,VER,TYPE,LEN */
        if (d->buf[1] != LP_VERSION || d->buf[3] > LP_MAX_PAYLOAD) {
            drop_front(d, 1);                      /* false anchor */
            continue;
        }
        const uint8_t plen = d->buf[3];
        const uint8_t total = (uint8_t)(6u + plen);
        if (d->len < total) return false;          /* wait for the rest */

        const uint16_t calc = lp_crc16(&d->buf[1], (size_t)3 + plen);
        const uint16_t rx = (uint16_t)d->buf[4 + plen] |
                            ((uint16_t)d->buf[5 + plen] << 8);
        if (rx != calc) { drop_front(d, 1); continue; } /* bad frame: resync */

        const uint8_t type = d->buf[2];
        bool ok = decode_payload(type, plen, &d->buf[4], out);
        drop_front(d, total);
        if (ok) return true;
        /* known framing but unusable (unknown/short type): already consumed. */
    }
}
