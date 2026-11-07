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

/* lp_config_t wire layout (30 bytes), identical to link_proto's. */
#define CFG_LEN 30u
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
#define NODE_STATUS_LEN (37u + LP_FW_LEN)   /* 61 <= FP_MAX_PAYLOAD */

size_t fp_encode_node_status(uint8_t *buf, size_t cap, const fp_node_status_t *m) {
    uint8_t pl[NODE_STATUS_LEN];
    pl[0] = m->gate_index;
    pl[1] = m->channel;
    put_i32(pl + 2, m->cn_x10);
    pl[6] = m->state;
    put_u32(pl + 7, m->passes);
    pl[11] = m->flags;
    put_u32(pl + 12, m->seq);
    pl[16] = m->gain;
    put_u32(pl + 17, m->uptime_s);
    put_u32(pl + 21, m->heap_min);
    put_u32(pl + 25, m->faults);
    for (unsigned k = 0; k < LP_LEVELS; k++) pl[29 + k] = (uint8_t)m->levels[k];
    /* NUL-padded, never unterminated: the last byte is always 0. */
    unsigned i = 0;
    for (; i + 1u < LP_FW_LEN && m->fw[i]; i++) pl[37 + i] = (uint8_t)m->fw[i];
    for (; i < LP_FW_LEN; i++) pl[37 + i] = 0;
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
        out->node_status.state = p[6];
        out->node_status.passes = get_u32(p + 7);
        out->node_status.flags = p[11];
        out->node_status.seq = get_u32(p + 12);
        out->node_status.gain = p[16];
        out->node_status.uptime_s = get_u32(p + 17);
        out->node_status.heap_min = get_u32(p + 21);
        out->node_status.faults = get_u32(p + 25);
        for (unsigned k = 0; k < LP_LEVELS; k++) out->node_status.levels[k] = (int8_t)p[29 + k];
        for (unsigned i = 0; i < LP_FW_LEN; i++) out->node_status.fw[i] = (char)p[37 + i];
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
    default:
        return false;
    }
}
