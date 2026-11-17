/* Host test for components/fleet_proto. Build/run:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/fleet_proto/include \
 *      -I../components/link_proto/include test_fleet_proto.c \
 *      ../components/fleet_proto/fleet_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
 */
#include "fleet_proto.h"
#include <stdio.h>
#include <string.h>

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
                     .stale_ms = 1000, .rearm_db_x10 = LP_REARM_DEFAULT_X10, .volume_pct = 60, .muted = 0 } };
        size_t n = fp_encode_config(buf, sizeof buf, &s);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_CONFIG &&
                  m.config.gate_index == 1 && m.config.channel == 36 && m.config.enable == 1 &&
                  m.config.det.active_db_x10 == 150 && m.config.det.refractory_ms == 1500u &&
                  m.config.det.stale_ms == 1000u && m.config.det.rearm_db_x10 == LP_REARM_DEFAULT_X10 &&
                  m.config.det.volume_pct == 60;
        CHECK(ok, "config roundtrip (embeds lp_config_t, the re-arm drop included)");
        CHECK(n == 4u + 3u + 34u, "config frame: 3 bytes + the 34-byte lp_config_t");
        s.det.rearm_db_x10 = 0;
        n = fp_encode_config(buf, sizeof buf, &s);
        ok = n && fp_decode(buf, n, &m) && m.config.det.rearm_db_x10 == 0 && m.config.det.volume_pct == 60;
        s.det.rearm_db_x10 = LP_REARM_MAX_X10;
        n = fp_encode_config(buf, sizeof buf, &s);
        ok = ok && n && fp_decode(buf, n, &m) && m.config.det.rearm_db_x10 == LP_REARM_MAX_X10;
        CHECK(ok, "config: a re-arm drop of 0 (off) and 40 dB round-trip");
        /* link_proto's CONFIG_SET takes the same lp_config_t: what the node forwards is what came. */
        uint8_t lb[LP_MAX_FRAME];
        lp_decoder_t d;
        lp_decoder_init(&d);
        const size_t ln = lp_encode_config(lb, sizeof lb, LP_MSG_CONFIG_SET, &m.config.det);
        lp_msg_t lm = {0};
        bool got = false;
        for (size_t i = 0; i < ln; i++) got = lp_decoder_push(&d, lb[i], &lm) || got;
        CHECK(got && lm.config.rearm_db_x10 == LP_REARM_MAX_X10 && lm.config.stale_ms == 1000u,
              "config: forwarded over link_proto unchanged");
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
                               .state = 2, .passes = 9, .flags = LP_HEALTH_CAL_NEEDED,
                               .seq = 0xDEADBEEF, .gain = 76, .uptime_s = 3600,
                               .heap_min = 131072, .faults = 3, .levels = {1, -2, 3, LP_LEVEL_UNKNOWN, 5, 6, 127, -127},
                               .rearm_db_x10 = 140, .fw = "5.0.12+abcdef123456" };
        size_t n = fp_encode_node_status(buf, sizeof buf, &s);
        const fp_node_status_t *r = &m.node_status;
        bool ok = n && n <= FP_MAX_FRAME && fp_decode(buf, n, &m) && m.type == FP_MSG_NODE_STATUS &&
                  r->cn_x10 == LP_CN_UNKNOWN && r->passes == 9 && r->state == 2 &&
                  r->flags == LP_HEALTH_CAL_NEEDED && r->seq == 0xDEADBEEF && r->gain == 76 &&
                  r->uptime_s == 3600 && r->heap_min == 131072 && r->faults == 3 &&
                  memcmp(r->levels, s.levels, LP_LEVELS) == 0 && r->rearm_db_x10 == 140 &&
                  strcmp(r->fw, "5.0.12+abcdef123456") == 0;
        CHECK(ok, "node_status roundtrip (negative cn, seq, C5 health, re-arm drop, fw id)");
        CHECK(n == 4u + 63u && n <= FP_MAX_FRAME, "node_status: 63-byte payload, within FP_MAX_PAYLOAD");
        s.rearm_db_x10 = FP_REARM_UNKNOWN;
        n = fp_encode_node_status(buf, sizeof buf, &s);
        ok = n && fp_decode(buf, n, &m) && r->rearm_db_x10 == FP_REARM_UNKNOWN &&
             memcmp(r->levels, s.levels, LP_LEVELS) == 0 && strcmp(r->fw, "5.0.12+abcdef123456") == 0;
        CHECK(ok, "node_status: an unknown re-arm drop (no echo yet) round-trips");

        memset(s.fw, 'x', sizeof s.fw);
        n = fp_encode_node_status(buf, sizeof buf, &s);
        ok = n && fp_decode(buf, n, &m) && strlen(m.node_status.fw) == LP_FW_LEN - 1u;
        CHECK(ok, "node_status fw id never unterminated");
    }
    {
        fp_command_t c = { .gate_index = 3, .cmd = LP_CMD_CAL_FULL };
        size_t n = fp_encode_command(buf, sizeof buf, &c);
        bool ok = n && fp_decode(buf, n, &m) && m.type == FP_MSG_COMMAND &&
                  m.command.gate_index == 3 && m.command.cmd == LP_CMD_CAL_FULL;
        c.cmd = LP_CMD_REBOOT;
        n = fp_encode_command(buf, sizeof buf, &c);
        ok = ok && n && fp_decode(buf, n, &m) && m.command.cmd == LP_CMD_REBOOT;
        CHECK(ok, "command roundtrip (cal_full, reboot)");
        c.cmd = 0;
        CHECK(fp_encode_command(buf, sizeof buf, &c) == 0, "encode_command refuses an unknown cmd");
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
    /* A version-3 board (no re-arm drop) is refused, not misread, even with a valid CRC. */
    {
        fp_config_t s = { .gate_index = 1, .enable = 1, .det = { .alpha_milli = 250, .stale_ms = 1000 } };
        size_t n = fp_encode_config(buf, sizeof buf, &s);
        buf[0] = 3;
        const uint16_t crc = lp_crc16(buf, n - 2u);
        buf[n - 2] = (uint8_t)(crc & 0xFFu);
        buf[n - 1] = (uint8_t)(crc >> 8);
        CHECK(FP_VERSION == 4u && !fp_decode(buf, n, &m), "version-3 frame rejected (valid CRC)");
        /* Its 33-byte CONFIG payload under version 4 is the wrong length. */
        n = fp_encode_config(buf, sizeof buf, &s) - 4u;   /* drop the last 4 payload bytes */
        const uint16_t c2 = lp_crc16(buf, n - 2u);
        buf[n - 2] = (uint8_t)(c2 & 0xFFu);
        buf[n - 1] = (uint8_t)(c2 >> 8);
        CHECK(!fp_decode(buf, n, &m), "a v3-length config payload rejected at version 4");
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
