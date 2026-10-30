/* Host test for components/fleet_proto. Build/run:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/fleet_proto/include \
 *      -I../components/link_proto/include test_fleet_proto.c \
 *      ../components/fleet_proto/fleet_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
 */
#include "fleet_proto.h"
#include <stdio.h>

static int g_pass, g_fail;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { printf("ok   %s\n", name); g_pass++; }                    \
        else { printf("FAIL %s\n", name); g_fail++; }                         \
    } while (0)

int main(void) {
    uint8_t buf[FP_MAX_FRAME];
    fp_msg_t m;

    {
        fp_pair_request_t s = { .nonce = 0xDEADBEEF, .seen_channel = 27, .fw_major = 5 };
        size_t n = fp_encode_pair_request(buf, sizeof buf, &s);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_PAIR_REQUEST &&
                  m.pair_request.nonce == 0xDEADBEEF && m.pair_request.seen_channel == 27 &&
                  m.pair_request.fw_major == 5;
        CHECK(ok, "pair_request roundtrip");
    }
    {
        fp_pair_accept_t s = { .nonce = 0x01020304, .node_id = 3, .gate_index = 2, .channel = 40 };
        size_t n = fp_encode_pair_accept(buf, sizeof buf, &s);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_PAIR_ACCEPT &&
                  m.pair_accept.nonce == 0x01020304 && m.pair_accept.node_id == 3 &&
                  m.pair_accept.gate_index == 2 && m.pair_accept.channel == 40;
        CHECK(ok, "pair_accept roundtrip");
    }
    {
        fp_time_beacon_t s = { .seq = 77, .fleet_ms = 123456789 };
        size_t n = fp_encode_time_beacon(buf, sizeof buf, &s);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_TIME_BEACON &&
                  m.time_beacon.seq == 77 && m.time_beacon.fleet_ms == 123456789u;
        CHECK(ok, "time_beacon roundtrip");
    }
    {
        fp_config_t s = { .gate_index = 1, .channel = 36, .enable = 1,
            .det = { .alpha_milli = 300, .active_db_x10 = 150, .hysteresis_db_x10 = 30,
                     .drop_db_x10 = 30, .min_peak_db_x10 = 250, .refractory_ms = 1500,
                     .stale_ms = 1000, .volume_pct = 60, .muted = 0 } };
        size_t n = fp_encode_config(buf, sizeof buf, &s);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_CONFIG &&
                  m.config.gate_index == 1 && m.config.channel == 36 && m.config.enable == 1 &&
                  m.config.det.active_db_x10 == 150 && m.config.det.refractory_ms == 1500u &&
                  m.config.det.volume_pct == 60;
        CHECK(ok, "config roundtrip (embeds lp_config_t)");
    }
    {
        fp_gate_pass_t s = { .gate_index = 4, .channel = 40, .peak_db_x10 = 352,
                             .fleet_ms = 900100, .seq = 12 };
        size_t n = fp_encode_gate_pass(buf, sizeof buf, &s);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_GATE_PASS &&
                  m.gate_pass.gate_index == 4 && m.gate_pass.channel == 40 &&
                  m.gate_pass.peak_db_x10 == 352 && m.gate_pass.fleet_ms == 900100u &&
                  m.gate_pass.seq == 12;
        CHECK(ok, "gate_pass roundtrip");
    }
    {
        fp_node_status_t s = { .gate_index = 0, .channel = 36, .cn_x10 = LP_CN_UNKNOWN,
                               .state = 2, .passes = 9, .flags = 1 };
        size_t n = fp_encode_node_status(buf, sizeof buf, &s);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_NODE_STATUS &&
                  m.node_status.cn_x10 == LP_CN_UNKNOWN && m.node_status.passes == 9 &&
                  m.node_status.state == 2 && m.node_status.flags == 1;
        CHECK(ok, "node_status roundtrip (negative cn)");
    }
    {
        fp_heartbeat_t s = { .role = FP_ROLE_MASTER, .uptime_ms = 42 };
        size_t n = fp_encode_heartbeat(buf, sizeof buf, &s);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_HEARTBEAT &&
                  m.heartbeat.role == FP_ROLE_MASTER && m.heartbeat.uptime_ms == 42;
        CHECK(ok, "heartbeat roundtrip");
    }

    /* A flipped byte fails the CRC. */
    {
        fp_heartbeat_t s = { .role = FP_ROLE_NODE, .uptime_ms = 1 };
        size_t n = fp_encode_heartbeat(buf, sizeof buf, &s);
        buf[2] ^= 0xFF;
        CHECK(!fp_decode(buf, n, &m), "corrupted frame rejected by crc");
    }
    /* Wrong version byte is rejected. */
    {
        fp_heartbeat_t s = { .role = FP_ROLE_NODE, .uptime_ms = 1 };
        size_t n = fp_encode_heartbeat(buf, sizeof buf, &s);
        buf[0] = 0x99;
        CHECK(!fp_decode(buf, n, &m), "wrong version rejected");
    }
    /* A truncated frame (wrong length for the type) is rejected. */
    {
        fp_gate_pass_t s = { .gate_index = 1, .channel = 1, .peak_db_x10 = 1,
                             .fleet_ms = 1, .seq = 1 };
        size_t n = fp_encode_gate_pass(buf, sizeof buf, &s);
        CHECK(!fp_decode(buf, n - 1, &m), "truncated frame rejected");
    }
    /* Encoding into a one-byte-short buffer fails cleanly. */
    {
        fp_heartbeat_t s = { .role = FP_ROLE_NODE, .uptime_ms = 1 };
        size_t full = fp_encode_heartbeat(buf, sizeof buf, &s);
        CHECK(fp_encode_heartbeat(buf, full - 1, &s) == 0, "encode refuses a too-small buffer");
    }

    printf("%d/%d fleet_proto checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
