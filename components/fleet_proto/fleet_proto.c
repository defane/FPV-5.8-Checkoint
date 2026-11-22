/* fleet_proto: C6 <-> C3 ESP-NOW messages. See fleet_proto.h. */
#include "fleet_proto.h"

/* ---- little-endian field helpers ---- */

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void put_i32(uint8_t *p, int32_t v) { put_u32(p, (uint32_t)v); }
static int32_t get_i32(const uint8_t *p) { return (int32_t)get_u32(p); }

/* lp_config_t wire layout (34 bytes), identical to link_proto's. */
#define CFG_LEN 34u
static void put_config(uint8_t *p, const lp_config_t *c) {
    put_i32(p + 0,  c->alpha_milli);
    put_i32(p + 4,  c->active_db_x10);
    put_i32(p + 8,  c->hysteresis_db_x10);
    put_i32(p + 12, c->drop_db_x10);
    put_i32(p + 16, c->min_peak_db_x10);
    put_u32(p + 20, c->refractory_ms);
    put_u32(p + 24, c->stale_ms);
    put_i32(p + 28, c->rearm_db_x10);
    p[32] = c->volume_pct;
    p[33] = c->muted;
}
static void get_config(const uint8_t *p, lp_config_t *c) {
    c->alpha_milli       = get_i32(p + 0);
    c->active_db_x10     = get_i32(p + 4);
    c->hysteresis_db_x10 = get_i32(p + 8);
    c->drop_db_x10       = get_i32(p + 12);
    c->min_peak_db_x10   = get_i32(p + 16);
    c->refractory_ms     = get_u32(p + 20);
    c->stale_ms          = get_u32(p + 24);
    c->rearm_db_x10      = get_i32(p + 28);
    c->volume_pct        = p[32];
    c->muted             = p[33];
}

/* ---- framing ---- */

static size_t frame(uint8_t *buf, size_t cap, fp_type_t type,
                    const uint8_t *payload, uint8_t len) {
    const size_t total = 4u + (size_t)len;   /* VER+TYPE + payload + CRC16 */
    if (cap < total || len > FP_MAX_PAYLOAD) return 0;
    buf[0] = FP_VERSION;
    buf[1] = (uint8_t)type;
    for (uint8_t i = 0; i < len; i++) buf[2 + i] = payload[i];
    const uint16_t crc = lp_crc16(buf, (size_t)2 + len);   /* VER,TYPE,payload */
    buf[2 + len] = (uint8_t)(crc & 0xFFu);
    buf[3 + len] = (uint8_t)(crc >> 8);
    return total;
}

size_t fp_encode_pair_request(uint8_t *buf, size_t cap, const fp_pair_request_t *m) {
    uint8_t pl[6];
    put_u32(pl, m->nonce);
    pl[4] = m->seen_channel;
    pl[5] = m->fw_major;
    return frame(buf, cap, FP_MSG_PAIR_REQUEST, pl, sizeof pl);
}
size_t fp_encode_pair_accept(uint8_t *buf, size_t cap, const fp_pair_accept_t *m) {
    uint8_t pl[7];
    put_u32(pl, m->nonce);
    pl[4] = m->node_id;
    pl[5] = m->gate_index;
    pl[6] = m->channel;
    return frame(buf, cap, FP_MSG_PAIR_ACCEPT, pl, sizeof pl);
}
size_t fp_encode_time_beacon(uint8_t *buf, size_t cap, const fp_time_beacon_t *m) {
    uint8_t pl[8];
    put_u32(pl + 0, m->seq);
    put_u32(pl + 4, m->fleet_ms);
    return frame(buf, cap, FP_MSG_TIME_BEACON, pl, sizeof pl);
}
size_t fp_encode_config(uint8_t *buf, size_t cap, const fp_config_t *m) {
    uint8_t pl[3 + CFG_LEN];
    pl[0] = m->gate_index;
    pl[1] = m->channel;
    pl[2] = m->enable;
    put_config(pl + 3, &m->det);
    return frame(buf, cap, FP_MSG_CONFIG, pl, sizeof pl);
}
size_t fp_encode_gate_pass(uint8_t *buf, size_t cap, const fp_gate_pass_t *m) {
    uint8_t pl[14];
    pl[0] = m->gate_index;
    pl[1] = m->channel;
    put_i32(pl + 2, m->peak_db_x10);
    put_u32(pl + 6, m->fleet_ms);
    put_u32(pl + 10, m->seq);
    return frame(buf, cap, FP_MSG_GATE_PASS, pl, sizeof pl);
}
#define NODE_STATUS_LEN (40u + LP_FW_LEN)   /* 64 = FP_MAX_PAYLOAD: full */
_Static_assert(NODE_STATUS_LEN <= FP_MAX_PAYLOAD, "NODE_STATUS fits a fleet frame");

size_t fp_encode_node_status(uint8_t *buf, size_t cap, const fp_node_status_t *m) {
    uint8_t pl[NODE_STATUS_LEN];
    pl[0] = m->gate_index;
    pl[1] = m->channel;
    put_i32(pl + 2, m->cn_x10);
    pl[6] = (uint8_t)(m->states & 0xFFu);     /* R1..R4 */
    pl[7] = (uint8_t)(m->states >> 8);        /* R5..R8 */
    put_u32(pl + 8, m->passes);
    pl[12] = m->flags;
    put_u32(pl + 13, m->seq);
    pl[17] = m->gain;
    put_u32(pl + 18, m->uptime_s);
    put_u32(pl + 22, m->heap_min);
    put_u32(pl + 26, m->faults);
    for (unsigned k = 0; k < LP_LEVELS; k++) pl[30 + k] = (uint8_t)m->levels[k];
    pl[38] = (uint8_t)(m->rearm_db_x10 & 0xFFu);
    pl[39] = (uint8_t)(m->rearm_db_x10 >> 8);
    /* NUL-padded, never unterminated: the last byte is always 0. */
    unsigned i = 0;
    for (; i + 1u < LP_FW_LEN && m->fw[i]; i++) pl[40 + i] = (uint8_t)m->fw[i];
    for (; i < LP_FW_LEN; i++) pl[40 + i] = 0;
    return frame(buf, cap, FP_MSG_NODE_STATUS, pl, sizeof pl);
}

/* The commands link_proto forwards to a C5 (lp_encode_command validates). */
static bool cmd_known(uint8_t c) {
    uint8_t tmp[LP_MAX_FRAME];
    return lp_encode_command(tmp, sizeof tmp, (lp_cmd_t)c) != 0;
}

size_t fp_encode_command(uint8_t *buf, size_t cap, const fp_command_t *m) {
    if (!cmd_known(m->cmd)) return 0;
    const uint8_t pl[2] = { m->gate_index, m->cmd };
    return frame(buf, cap, FP_MSG_COMMAND, pl, sizeof pl);
}
size_t fp_encode_heartbeat(uint8_t *buf, size_t cap, const fp_heartbeat_t *m) {
    uint8_t pl[5];
    pl[0] = m->role;
    put_u32(pl + 1, m->uptime_ms);
    return frame(buf, cap, FP_MSG_HEARTBEAT, pl, sizeof pl);
}

size_t fp_encode_debug(uint8_t *buf, size_t cap, bool on) {
    const uint8_t pl = on ? 1u : 0u;
    return frame(buf, cap, FP_MSG_DEBUG, &pl, 1u);
}
size_t fp_encode_key(uint8_t *buf, size_t cap, const char *keys, size_t len) {
    if (!keys || len == 0 || len > LP_KEY_MAX) return 0;
    uint8_t pl[1 + LP_KEY_MAX];
    pl[0] = (uint8_t)len;
    for (size_t i = 0; i < len; i++) pl[1 + i] = (uint8_t)keys[i];
    return frame(buf, cap, FP_MSG_KEY, pl, (uint8_t)(1u + len));
}
size_t fp_encode_log_req(uint8_t *buf, size_t cap, uint32_t since) {
    uint8_t pl[4];
    put_u32(pl, since);
    return frame(buf, cap, FP_MSG_LOG_REQ, pl, sizeof pl);
}
#define LOG_CHUNK_HDR 6u   /* from(4) + flags + len */
_Static_assert(LOG_CHUNK_HDR + LP_TEXT_MAX <= FP_MAX_PAYLOAD, "a full LOG_CHUNK fits a fleet frame");
size_t fp_encode_log_chunk(uint8_t *buf, size_t cap, const fp_log_chunk_t *m) {
    if (m->len > LP_TEXT_MAX) return 0;
    uint8_t pl[LOG_CHUNK_HDR + LP_TEXT_MAX];
    put_u32(pl, m->from);
    pl[4] = m->flags;
    pl[5] = m->len;
    for (uint8_t i = 0; i < m->len; i++) pl[LOG_CHUNK_HDR + i] = (uint8_t)m->text[i];
    return frame(buf, cap, FP_MSG_LOG_CHUNK, pl, (uint8_t)(LOG_CHUNK_HDR + m->len));
}

bool fp_log_take(uint32_t *expected, const fp_log_chunk_t *c, uint8_t *skip, uint32_t *gap) {
    *skip = 0;
    *gap = 0;
    const uint32_t end = c->from + c->len;
    if (c->len == 0 || end <= *expected) return false;
    if (c->from > *expected) *gap = c->from - *expected;
    else *skip = (uint8_t)(*expected - c->from);
    *expected = end;
    return true;
}

/* ---- decode ---- */

bool fp_decode(const uint8_t *buf, size_t n, fp_msg_t *out) {
    if (n < 4u || n > FP_MAX_FRAME) return false;
    if (buf[0] != FP_VERSION) return false;
    const uint8_t len = (uint8_t)(n - 4u);
    const uint16_t calc = lp_crc16(buf, (size_t)2 + len);
    const uint16_t rx = (uint16_t)buf[2 + len] | ((uint16_t)buf[3 + len] << 8);
    if (rx != calc) return false;

    const uint8_t *p = &buf[2];
    out->type = (fp_type_t)buf[1];
    switch (buf[1]) {
    case FP_MSG_PAIR_REQUEST:
        if (len != 6) return false;
        out->pair_request.nonce = get_u32(p);
        out->pair_request.seen_channel = p[4];
        out->pair_request.fw_major = p[5];
        return true;
    case FP_MSG_PAIR_ACCEPT:
        if (len != 7) return false;
        out->pair_accept.nonce = get_u32(p);
        out->pair_accept.node_id = p[4];
        out->pair_accept.gate_index = p[5];
        out->pair_accept.channel = p[6];
        return true;
    case FP_MSG_TIME_BEACON:
        if (len != 8) return false;
        out->time_beacon.seq = get_u32(p + 0);
        out->time_beacon.fleet_ms = get_u32(p + 4);
        return true;
    case FP_MSG_CONFIG:
        if (len != 3 + CFG_LEN) return false;
        out->config.gate_index = p[0];
        out->config.channel = p[1];
        out->config.enable = p[2];
        get_config(p + 3, &out->config.det);
        return true;
    case FP_MSG_GATE_PASS:
        if (len != 14) return false;
        out->gate_pass.gate_index = p[0];
        out->gate_pass.channel = p[1];
        out->gate_pass.peak_db_x10 = get_i32(p + 2);
        out->gate_pass.fleet_ms = get_u32(p + 6);
        out->gate_pass.seq = get_u32(p + 10);
        return true;
    case FP_MSG_NODE_STATUS:
        if (len != NODE_STATUS_LEN) return false;
        out->node_status.gate_index = p[0];
        out->node_status.channel = p[1];
        out->node_status.cn_x10 = get_i32(p + 2);
        out->node_status.states = (uint16_t)(p[6] | ((uint16_t)p[7] << 8));
        out->node_status.passes = get_u32(p + 8);
        out->node_status.flags = p[12];
        out->node_status.seq = get_u32(p + 13);
        out->node_status.gain = p[17];
        out->node_status.uptime_s = get_u32(p + 18);
        out->node_status.heap_min = get_u32(p + 22);
        out->node_status.faults = get_u32(p + 26);
        for (unsigned k = 0; k < LP_LEVELS; k++) out->node_status.levels[k] = (int8_t)p[30 + k];
        out->node_status.rearm_db_x10 = (uint16_t)(p[38] | ((uint16_t)p[39] << 8));
        for (unsigned i = 0; i < LP_FW_LEN; i++) out->node_status.fw[i] = (char)p[40 + i];
        out->node_status.fw[LP_FW_LEN - 1u] = '\0';   /* a peer may not have terminated it */
        return true;
    case FP_MSG_COMMAND:
        if (len != 2 || !cmd_known(p[1])) return false;
        out->command.gate_index = p[0];
        out->command.cmd = p[1];
        return true;
    case FP_MSG_HEARTBEAT:
        if (len != 5) return false;
        out->heartbeat.role = p[0];
        out->heartbeat.uptime_ms = get_u32(p + 1);
        return true;
    case FP_MSG_DEBUG:
        if (len != 1 || p[0] > 1) return false;
        out->debug.on = p[0];
        return true;
    case FP_MSG_KEY:
        if (len < 2 || p[0] == 0 || p[0] > LP_KEY_MAX || len != 1u + p[0]) return false;
        out->key.len = p[0];
        for (uint8_t i = 0; i < p[0]; i++) out->key.text[i] = (char)p[1 + i];
        return true;
    case FP_MSG_LOG_REQ:
        if (len != 4) return false;
        out->log_req.since = get_u32(p);
        return true;
    case FP_MSG_LOG_CHUNK:
        if (len < LOG_CHUNK_HDR || p[5] > LP_TEXT_MAX || len != LOG_CHUNK_HDR + p[5]) return false;
        out->log_chunk.from = get_u32(p);
        out->log_chunk.flags = p[4];
        out->log_chunk.len = p[5];
        for (uint8_t i = 0; i < p[5]; i++) out->log_chunk.text[i] = (char)p[LOG_CHUNK_HDR + i];
        return true;
    default:
        return false;
    }
}
