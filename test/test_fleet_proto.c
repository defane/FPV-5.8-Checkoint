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
        /* R1..R8: every state twice, R1 and R8 COOLDOWN so both ends of the 16 bits are set. */
        const unsigned want[LP_LEVELS] = { LP_STATE_COOLDOWN, LP_STATE_IDLE, LP_STATE_RISING,
                                           LP_STATE_PEAK_PASSED, LP_STATE_PEAK_PASSED, LP_STATE_RISING,
                                           LP_STATE_IDLE, LP_STATE_COOLDOWN };
        uint16_t states = 0;
        for (unsigned ch = 0; ch < LP_LEVELS; ch++) states = lp_state_set(states, ch, want[ch]);
        fp_node_status_t s = { .gate_index = 2, .channel = 36, .cn_x10 = LP_CN_UNKNOWN,
                               .states = states, .passes = 9, .flags = LP_HEALTH_CAL_NEEDED,
                               .seq = 0xDEADBEEF, .gain = 76, .uptime_s = 3600,
                               .heap_min = 131072, .faults = 3, .levels = {1, -2, 3, LP_LEVEL_UNKNOWN, 5, 6, 127, -127},
                               .rearm_db_x10 = 140, .fw = "5.0.12+abcdef123456" };
        size_t n = fp_encode_node_status(buf, sizeof buf, &s);
        const fp_node_status_t *r = &m.node_status;
        bool ok = n && n <= FP_MAX_FRAME && fp_decode(buf, n, &m) && m.type == FP_MSG_NODE_STATUS &&
                  r->gate_index == 2 && r->channel == 36 &&
                  r->cn_x10 == LP_CN_UNKNOWN && r->passes == 9 && r->states == states &&
                  r->flags == LP_HEALTH_CAL_NEEDED && r->seq == 0xDEADBEEF && r->gain == 76 &&
                  r->uptime_s == 3600 && r->heap_min == 131072 && r->faults == 3 &&
                  memcmp(r->levels, s.levels, LP_LEVELS) == 0 && r->rearm_db_x10 == 140 &&
                  strcmp(r->fw, "5.0.12+abcdef123456") == 0;
        for (unsigned ch = 0; ch < LP_LEVELS; ch++) ok = ok && lp_state_of(r->states, ch) == want[ch];
        CHECK(ok, "node_status roundtrip (each channel's state, negative cn, seq, C5 health, re-arm drop, fw id)");
        CHECK(n == 4u + 64u && n == FP_MAX_FRAME, "node_status: 64-byte payload, a full FP_MAX_FRAME");
        CHECK(states == 0xC693u && buf[2 + 6] == 0x93u && buf[2 + 7] == 0xC6u,
              "node_status states: as STATS packs them, little-endian at payload 6..7");

        /* The 63-byte payload of a NODE_STATUS with one state byte is the wrong length now. */
        n = fp_encode_node_status(buf, sizeof buf, &s) - 1u;   /* drop the last payload byte */
        const uint16_t crc = lp_crc16(buf, n - 2u);
        buf[n - 2] = (uint8_t)(crc & 0xFFu);
        buf[n - 1] = (uint8_t)(crc >> 8);
        CHECK(!fp_decode(buf, n, &m), "a 63-byte node_status payload rejected");
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
        CHECK(FP_VERSION == 6u && !fp_decode(buf, n, &m), "version-3 frame rejected (valid CRC)");
        /* Its 33-byte CONFIG payload under version 6 is the wrong length. */
        n = fp_encode_config(buf, sizeof buf, &s) - 4u;   /* drop the last 4 payload bytes */
        const uint16_t c2 = lp_crc16(buf, n - 2u);
        buf[n - 2] = (uint8_t)(c2 & 0xFFu);
        buf[n - 1] = (uint8_t)(c2 >> 8);
        CHECK(!fp_decode(buf, n, &m), "a v3-length config payload rejected at version 6");
    }

    /* Debug mode of a remote gate: DEBUG, KEY, LOG_REQ, LOG_CHUNK. */
    {
        size_t n = fp_encode_debug(buf, sizeof buf, true);
        CHECK(fp_decode(buf, n, &m) && m.type == FP_MSG_DEBUG && m.debug.on == 1, "debug on roundtrip");
        n = fp_encode_debug(buf, sizeof buf, false);
        CHECK(fp_decode(buf, n, &m) && m.debug.on == 0, "debug off roundtrip");
        uint8_t f[5] = { FP_VERSION, FP_MSG_DEBUG, 2 };
        uint16_t crc = lp_crc16(f, 3);
        f[3] = (uint8_t)crc; f[4] = (uint8_t)(crc >> 8);
        CHECK(!fp_decode(f, sizeof f, &m), "debug value out of range rejected");

        n = fp_encode_key(buf, sizeof buf, "Ka", 2);
        CHECK(fp_decode(buf, n, &m) && m.type == FP_MSG_KEY && m.key.len == 2 && m.key.text[0] == 'K' &&
              m.key.text[1] == 'a', "key roundtrip");
        const char sixteen[LP_KEY_MAX + 1] = "0123456789abcdef";
        n = fp_encode_key(buf, sizeof buf, sixteen, LP_KEY_MAX);
        CHECK(n && fp_decode(buf, n, &m) && m.key.len == LP_KEY_MAX, "a full key string fits");
        CHECK(fp_encode_key(buf, sizeof buf, sixteen, LP_KEY_MAX + 1) == 0 &&
              fp_encode_key(buf, sizeof buf, "", 0) == 0, "key string over the maximum, or empty, refused");
        n = fp_encode_key(buf, sizeof buf, "Ka", 2);
        buf[2] = 5;                                          /* length byte lies: CRC refuses it ... */
        CHECK(!fp_decode(buf, n, &m), "a key frame with a wrong CRC rejected");
        uint8_t k[8] = { FP_VERSION, FP_MSG_KEY, 5, 'K', 'a' };   /* ... and a valid CRC with a lying length */
        crc = lp_crc16(k, 5);
        k[5] = (uint8_t)crc; k[6] = (uint8_t)(crc >> 8);
        CHECK(!fp_decode(k, 7, &m), "key length byte over its payload rejected");

        n = fp_encode_log_req(buf, sizeof buf, 123456u);
        CHECK(fp_decode(buf, n, &m) && m.type == FP_MSG_LOG_REQ && m.log_req.since == 123456u, "log request roundtrip");

        fp_log_chunk_t c = { .from = 4000u, .flags = FP_LOG_DEBUG | FP_LOG_LOST, .len = 5, .text = "PASS\n" };
        n = fp_encode_log_chunk(buf, sizeof buf, &c);
        CHECK(fp_decode(buf, n, &m) && m.type == FP_MSG_LOG_CHUNK && m.log_chunk.from == 4000u &&
              m.log_chunk.flags == (FP_LOG_DEBUG | FP_LOG_LOST) && m.log_chunk.len == 5 &&
              memcmp(m.log_chunk.text, "PASS\n", 5) == 0, "log chunk roundtrip");
        c.len = 0;
        n = fp_encode_log_chunk(buf, sizeof buf, &c);
        CHECK(fp_decode(buf, n, &m) && m.log_chunk.len == 0, "an empty log chunk (nothing new) roundtrips");
        c.len = LP_TEXT_MAX;
        memset(c.text, 'a', sizeof c.text);
        n = fp_encode_log_chunk(buf, sizeof buf, &c);
        CHECK(n && n <= FP_MAX_FRAME && fp_decode(buf, n, &m) && m.log_chunk.len == LP_TEXT_MAX,
              "a full log chunk fits a fleet frame");
        c.len = LP_TEXT_MAX + 1;
        CHECK(fp_encode_log_chunk(buf, sizeof buf, &c) == 0, "log chunk over the maximum refused");
        uint8_t l[12] = { FP_VERSION, FP_MSG_LOG_CHUNK, 0, 0, 0, 0, 0, 4, 'a', 'b' };   /* says 4, carries 2 */
        crc = lp_crc16(l, 10);
        l[10] = (uint8_t)crc; l[11] = (uint8_t)(crc >> 8);
        CHECK(!fp_decode(l, sizeof l, &m), "log chunk length byte over its payload rejected");
        n = fp_encode_log_req(buf, sizeof buf, 1u);
        CHECK(!fp_decode(buf, n - 1, &m), "a truncated log request rejected");
    }

    /* fp_log_take: the master keeps each byte of the node's log once, and counts a gap. */
    {
        uint32_t exp = 0;
        uint8_t skip;
        uint32_t gap;
        fp_log_chunk_t c = { .from = 0, .len = 10 };
        CHECK(fp_log_take(&exp, &c, &skip, &gap) && skip == 0 && gap == 0 && exp == 10, "the first chunk is kept whole");
        CHECK(!fp_log_take(&exp, &c, &skip, &gap) && exp == 10, "the same chunk again (a request asked twice) gives nothing");
        c.from = 6; c.len = 10;                                  /* 6..16: 4 bytes are repeats */
        CHECK(fp_log_take(&exp, &c, &skip, &gap) && skip == 4 && gap == 0 && exp == 16, "an overlapping chunk gives its tail");
        c.from = 40; c.len = 5;                                  /* the node's ring dropped 16..40 */
        CHECK(fp_log_take(&exp, &c, &skip, &gap) && skip == 0 && gap == 24 && exp == 45, "a later chunk reports the bytes lost");
        c.from = 45; c.len = 0;
        CHECK(!fp_log_take(&exp, &c, &skip, &gap) && exp == 45 && gap == 0, "an empty chunk (nothing new) changes nothing");
        c.from = 3; c.len = 5;
        CHECK(!fp_log_take(&exp, &c, &skip, &gap) && exp == 45, "an older chunk (a late reply) is dropped");
    }

    /* GAIN: the master forces a gate's RX gain. */
    {
        const fp_gain_t gm = { .gate_index = 3, .gain = 44 };
        size_t n = fp_encode_gain(buf, sizeof buf, &gm);
        CHECK(fp_decode(buf, n, &m) && m.type == FP_MSG_GAIN && m.gain.gate_index == 3 && m.gain.gain == 44, "gain roundtrip");
        const fp_gain_t lo = { .gate_index = 1, .gain = LP_GAIN_MIN - 1 }, hi = { .gate_index = 1, .gain = LP_GAIN_MAX + 1 };
        CHECK(fp_encode_gain(buf, sizeof buf, &lo) == 0 && fp_encode_gain(buf, sizeof buf, &hi) == 0, "gain outside the bounds refused");
        uint8_t f[6] = { FP_VERSION, FP_MSG_GAIN, 1, 200 };
        uint16_t crc = lp_crc16(f, 4);
        f[4] = (uint8_t)crc; f[5] = (uint8_t)(crc >> 8);
        CHECK(!fp_decode(f, sizeof f, &m), "gain out of range rejected (valid CRC)");
        n = fp_encode_gain(buf, sizeof buf, &gm);
        CHECK(!fp_decode(buf, n - 1, &m), "a truncated gain frame rejected");
    }

    /* CALIB_START / CALIB_STATE: the passage calibration, relayed. */
    {
        size_t n = fp_encode_calib_start(buf, sizeof buf, 1);
        CHECK(fp_decode(buf, n, &m) && m.type == FP_MSG_CALIB_START && m.calib_step == 1, "calib start roundtrip");
        CHECK(fp_encode_calib_start(buf, sizeof buf, 3) == 0, "calib step 3 refused");
        lp_calib_t c = { .phase = LP_CALIB_DONE, .flags = 0x0004, .channel = 2, .gate = { 30, 40, 50, 52, 60 },
                         .gate_sat_pct = 3, .away = { -2, 10, 20, 24, 30 }, .active_db_x10 = 105, .hysteresis_db_x10 = 40,
                         .drop_db_x10 = 30, .min_peak_db_x10 = 125, .gain_steps = 1 };
        n = fp_encode_calib_state(buf, sizeof buf, &c);
        CHECK(n && n <= FP_MAX_FRAME && fp_decode(buf, n, &m) && m.type == FP_MSG_CALIB_STATE && m.calib.phase == LP_CALIB_DONE &&
              m.calib.away[0] == -2 && m.calib.min_peak_db_x10 == 125 && m.calib.gain_steps == 1 && m.calib.channel == 2,
              "calib state roundtrip through a fleet frame");
        c.phase = LP_CALIB_DONE + 1;
        CHECK(fp_encode_calib_state(buf, sizeof buf, &c) == 0, "calib state with an unknown phase refused");
        c.phase = LP_CALIB_MEASURED1;
        n = fp_encode_calib_state(buf, sizeof buf, &c);
        CHECK(!fp_decode(buf, n - 1, &m), "a truncated calib state rejected");
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
