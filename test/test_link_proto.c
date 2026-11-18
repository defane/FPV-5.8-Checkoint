/* Host test for components/link_proto. Build/run (see CLAUDE.md):
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/link_proto/include \
 *      test_link_proto.c ../components/link_proto/link_proto.c -o /tmp/t && /tmp/t
 */
#include "link_proto.h"
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { printf("ok   %s\n", name); g_pass++; }                    \
        else { printf("FAIL %s\n", name); g_fail++; }                         \
    } while (0)

/* Feed a byte buffer through a fresh decoder; store every decoded message in
 * out[], return how many completed. */
static int run(const uint8_t *bytes, size_t n, lp_msg_t *out, int max) {
    lp_decoder_t d;
    lp_decoder_init(&d);
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        lp_msg_t m;
        if (lp_decoder_push(&d, bytes[i], &m) && got < max) out[got++] = m;
    }
    return got;
}

static lp_config_t sample_config(void) {
    lp_config_t c = {
        .alpha_milli = 300, .active_db_x10 = 150, .hysteresis_db_x10 = 30,
        .drop_db_x10 = 30, .min_peak_db_x10 = 250, .refractory_ms = 1500,
        .stale_ms = 1000, .rearm_db_x10 = 155, .volume_pct = 60, .muted = 0,
    };
    return c;
}

/* Re-sign a frame after a test edited its header or payload. */
static void fix_crc(uint8_t *f, size_t n)
{
    const uint16_t crc = lp_crc16(&f[1], 3u + f[3]);
    f[n - 2] = (uint8_t)(crc & 0xFFu);
    f[n - 1] = (uint8_t)(crc >> 8);
}

int main(void) {
    uint8_t buf[LP_MAX_FRAME];
    lp_msg_t got[4];

    /* CRC16-CCITT known vector: "123456789" -> 0x29B1. */
    CHECK(lp_crc16((const uint8_t *)"123456789", 9) == 0x29B1, "crc16 reference vector");

    /* HELLO roundtrip. */
    {
        size_t n = lp_encode_hello(buf, sizeof buf, LP_ROLE_C5);
        int g = run(buf, n, got, 4);
        CHECK(g == 1 && got[0].type == LP_MSG_HELLO && got[0].role == LP_ROLE_C5,
              "hello roundtrip");
    }

    /* CONFIG_SET roundtrip, field by field. */
    {
        lp_config_t c = sample_config();
        size_t n = lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_SET, &c);
        int g = run(buf, n, got, 4);
        const lp_config_t *r = &got[0].config;
        bool ok = g == 1 && got[0].type == LP_MSG_CONFIG_SET &&
                  r->alpha_milli == c.alpha_milli &&
                  r->active_db_x10 == c.active_db_x10 &&
                  r->hysteresis_db_x10 == c.hysteresis_db_x10 &&
                  r->drop_db_x10 == c.drop_db_x10 &&
                  r->min_peak_db_x10 == c.min_peak_db_x10 &&
                  r->refractory_ms == c.refractory_ms &&
                  r->stale_ms == c.stale_ms && r->rearm_db_x10 == c.rearm_db_x10 &&
                  r->volume_pct == c.volume_pct && r->muted == c.muted;
        CHECK(ok, "config_set roundtrip (all fields)");
        CHECK(n == 6u + 34u, "config frame: 34-byte payload (v4: the re-arm drop)");
    }

    /* The re-arm drop: the codec carries any value (the C5 validates 0..LP_REARM_MAX_X10), its
     * four bytes in place, and leaves the fields around it alone. */
    {
        const int32_t drops[] = { 0, LP_REARM_DEFAULT_X10, LP_REARM_MAX_X10, -1, 0x12345678 };
        bool ok = true;
        for (unsigned k = 0; k < sizeof drops / sizeof drops[0]; k++) {
            lp_config_t c = sample_config();
            c.rearm_db_x10 = drops[k];
            size_t n = lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_STATE, &c);
            int g = run(buf, n, got, 4);
            ok = ok && g == 1 && got[0].config.rearm_db_x10 == drops[k] &&
                 got[0].config.stale_ms == c.stale_ms && got[0].config.volume_pct == c.volume_pct &&
                 got[0].config.muted == c.muted;
        }
        lp_config_t c = sample_config();
        c.rearm_db_x10 = 0x12345678;
        size_t n = lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_SET, &c);
        ok = ok && n && buf[4 + 28] == 0x78 && buf[4 + 31] == 0x12 && buf[4 + 32] == 60;   /* little endian, then volume */
        CHECK(ok, "config: the re-arm drop round-trips (0, 160, 400, -1, any), in place");
        CHECK(LP_REARM_DEFAULT_X10 == 160 && LP_REARM_MAX_X10 == 400, "re-arm drop: 16 dB default, 40 dB ceiling");
    }

    /* A version-3 CONFIG_SET (30-byte payload, no re-arm drop) is not misread: its version is
     * refused, and the same short payload under version 4 is too. */
    {
        lp_config_t c = sample_config();
        size_t n = lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_SET, &c);
        buf[1] = 3;
        fix_crc(buf, n);
        CHECK(run(buf, n, got, 4) == 0, "version-3 config frame rejected");
        (void)lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_SET, &c);
        buf[3] = 30;                                              /* a v3 payload length */
        n = 6u + 30u;
        fix_crc(buf, n);                                          /* the CRC where a v3 frame has it */
        CHECK(run(buf, n, got, 4) == 0, "a 30-byte config payload rejected at version 4");
    }

    /* CONFIG_STATE uses the same payload; CONFIG_REQ carries none. */
    {
        lp_config_t c = sample_config();
        c.muted = 1; c.volume_pct = 100;
        size_t n = lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_STATE, &c);
        int g = run(buf, n, got, 4);
        CHECK(g == 1 && got[0].type == LP_MSG_CONFIG_STATE &&
                  got[0].config.muted == 1 && got[0].config.volume_pct == 100,
              "config_state roundtrip");
        n = lp_encode_config_req(buf, sizeof buf);
        g = run(buf, n, got, 4);
        CHECK(g == 1 && got[0].type == LP_MSG_CONFIG_REQ, "config_req roundtrip");
    }

    /* lp_encode_config refuses a non-config type. */
    CHECK(lp_encode_config(buf, sizeof buf, LP_MSG_STATS, &(lp_config_t){0}) == 0,
          "encode_config refuses a non-config type");

    /* The states helpers: every value on every channel, the others untouched; past R8 is IDLE. */
    {
        bool ok = true;
        for (unsigned ch = 0; ch < LP_LEVELS; ch++)
            for (unsigned v = 0; v < 4u; v++) {
                const uint16_t st = lp_state_set(0xFFFFu, ch, v);
                ok = ok && lp_state_of(st, ch) == v && lp_state_of(lp_state_set(0, ch, v), ch) == v;
                for (unsigned o = 0; o < LP_LEVELS; o++)
                    ok = ok && (o == ch || lp_state_of(st, o) == LP_STATE_COOLDOWN);
            }
        ok = ok && lp_state_of(0x0003u, 0) == LP_STATE_COOLDOWN && lp_state_of(0xC000u, 7) == LP_STATE_COOLDOWN &&
             lp_state_of(0xFFFFu, LP_LEVELS) == LP_STATE_IDLE && lp_state_set(0x1234u, LP_LEVELS, 3) == 0x1234u &&
             lp_state_set(0, 2, 7) == lp_state_set(0, 2, 3);
        CHECK(ok, "lp_state_set / lp_state_of: each channel's 2 bits, R1 low, R8 high");
    }

    /* EVENT_PASS and STATS roundtrip. */
    {
        lp_event_t e = { .channel = 5, .peak_db_x10 = 352, .at_ms = 123456, .age_ms = 60123 };
        size_t n = lp_encode_event(buf, sizeof buf, &e);
        int g = run(buf, n, got, 4);
        bool ok = g == 1 && got[0].type == LP_MSG_EVENT_PASS &&
                  got[0].event.channel == 5 && got[0].event.peak_db_x10 == 352 &&
                  got[0].event.at_ms == 123456u && got[0].event.age_ms == 60123u;
        CHECK(ok, "event_pass roundtrip");

        /* R1..R8: every state twice, R1 and R8 COOLDOWN so both ends of the 16 bits are set. */
        const unsigned want[LP_LEVELS] = { LP_STATE_COOLDOWN, LP_STATE_IDLE, LP_STATE_RISING,
                                           LP_STATE_PEAK_PASSED, LP_STATE_PEAK_PASSED, LP_STATE_RISING,
                                           LP_STATE_IDLE, LP_STATE_COOLDOWN };
        uint16_t states = 0;
        for (unsigned ch = 0; ch < LP_LEVELS; ch++) states = lp_state_set(states, ch, want[ch]);
        lp_stats_t s = { .cn_x10 = LP_CN_UNKNOWN, .states = states,
                         .flags = LP_HEALTH_CAL_NEEDED | LP_HEALTH_CALIBRATING, .gain = 83,
                         .passes = 41, .uptime_s = 86400, .heap_min = 123456,
                         .faults = 0xFFFFFFFEu, .levels = {0, 1, -2, 127, -127, LP_LEVEL_UNKNOWN, 7, 8},
                         .fw = "5.0.0-dev+3201b7196ece" };
        n = lp_encode_stats(buf, sizeof buf, &s);
        g = run(buf, n, got, 4);
        const lp_stats_t *r = &got[0].stats;
        ok = g == 1 && got[0].type == LP_MSG_STATS && n == LP_MAX_FRAME && buf[3] == 56u &&
             r->cn_x10 == LP_CN_UNKNOWN && r->states == states &&
             r->flags == (LP_HEALTH_CAL_NEEDED | LP_HEALTH_CALIBRATING) && r->gain == 83 &&
             r->passes == 41 && r->uptime_s == 86400 && r->heap_min == 123456 &&
             r->faults == 0xFFFFFFFEu && memcmp(r->levels, s.levels, LP_LEVELS) == 0 &&
             strcmp(r->fw, "5.0.0-dev+3201b7196ece") == 0;
        for (unsigned ch = 0; ch < LP_LEVELS; ch++) ok = ok && lp_state_of(r->states, ch) == want[ch];
        CHECK(ok, "stats roundtrip (each channel's state, health, negative cn, fw id, 56-byte payload)");
        CHECK(states == 0xC693u && buf[4 + 4] == 0x93u && buf[4 + 5] == 0xC6u,
              "stats states: 2 bits per channel, R1 in bits 1..0, little-endian at payload 4..5");

        /* A fw id filling the whole field is truncated to stay NUL-terminated. */
        memset(s.fw, 'x', sizeof s.fw);
        n = lp_encode_stats(buf, sizeof buf, &s);
        g = run(buf, n, got, 4);
        ok = g == 1 && strlen(got[0].stats.fw) == LP_FW_LEN - 1u;
        CHECK(ok, "stats fw id never unterminated (encoder)");

        /* A peer that sends 24 non-NUL bytes cannot unterminate it either. */
        n = lp_encode_stats(buf, sizeof buf, &s);
        buf[4 + 32 + LP_FW_LEN - 1u] = 'y';                       /* last fw byte */
        fix_crc(buf, n);
        g = run(buf, n, got, 4);
        ok = g == 1 && got[0].stats.fw[LP_FW_LEN - 1u] == '\0';
        CHECK(ok, "stats fw id never unterminated (decoder)");
    }


    /* COMMAND: every known command roundtrips; an unknown one is neither
     * encoded nor decoded. */
    {
        const lp_cmd_t cmds[] = { LP_CMD_CAL_FULL, LP_CMD_CAL_QUICK, LP_CMD_REBOOT };
        bool ok = true;
        for (unsigned k = 0; k < sizeof cmds / sizeof cmds[0]; k++) {
            size_t n = lp_encode_command(buf, sizeof buf, cmds[k]);
            int g = run(buf, n, got, 4);
            ok = ok && n && g == 1 && got[0].type == LP_MSG_COMMAND && got[0].command == cmds[k];
        }
        CHECK(ok, "command roundtrip (cal_full, cal_quick, reboot)");
        CHECK(lp_encode_command(buf, sizeof buf, (lp_cmd_t)0) == 0 &&
              lp_encode_command(buf, sizeof buf, (lp_cmd_t)9) == 0, "encode_command refuses an unknown cmd");

        size_t n = lp_encode_command(buf, sizeof buf, LP_CMD_REBOOT);
        buf[4] = 9;                                                /* forge an unknown op */
        fix_crc(buf, n);
        CHECK(run(buf, n, got, 4) == 0, "unknown command dropped by the decoder");
    }

    /* A version-1 frame (pre-health STATS) is rejected outright, not misread. */
    {
        lp_stats_t s = { .cn_x10 = 100 };
        size_t n = lp_encode_stats(buf, sizeof buf, &s);
        buf[1] = 1;
        fix_crc(buf, n);
        CHECK(run(buf, n, got, 4) == 0, "version-1 frame rejected");
    }

    /* Encoding into a buffer one byte too small fails cleanly. */
    {
        lp_config_t c = sample_config();
        size_t full = lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_SET, &c);
        CHECK(lp_encode_config(buf, full - 1, LP_MSG_CONFIG_SET, &c) == 0,
              "encode refuses a too-small buffer");
    }

    /* Leading noise then a valid frame: the decoder resynchronises. */
    {
        uint8_t stream[8 + LP_MAX_FRAME];
        const uint8_t noise[] = { 0x00, 0xFF, 0xC5, 0x42, 0x13, 0x37 };
        memcpy(stream, noise, sizeof noise);
        lp_event_t e = { .channel = 1, .peak_db_x10 = 300, .at_ms = 10 };
        size_t n = lp_encode_event(stream + sizeof noise, LP_MAX_FRAME, &e);
        int g = run(stream, sizeof noise + n, got, 4);
        CHECK(g == 1 && got[0].type == LP_MSG_EVENT_PASS && got[0].event.channel == 1,
              "resync after leading noise");
    }

    /* A flipped payload byte fails the CRC and yields nothing. */
    {
        lp_event_t e = { .channel = 2, .peak_db_x10 = 300, .at_ms = 10 };
        size_t n = lp_encode_event(buf, sizeof buf, &e);
        buf[4] ^= 0xFF; /* corrupt channel byte */
        int g = run(buf, n, got, 4);
        CHECK(g == 0, "corrupted payload rejected by crc");
    }

    /* A truncated frame followed by a whole one: only the second decodes. */
    {
        uint8_t stream[LP_MAX_FRAME * 2];
        lp_event_t e = { .channel = 4, .peak_db_x10 = 300, .at_ms = 99 };
        size_t full = lp_encode_event(buf, sizeof buf, &e);
        size_t cut = full - 2; /* drop the CRC: incomplete */
        memcpy(stream, buf, cut);
        size_t n2 = lp_encode_event(stream + cut, LP_MAX_FRAME, &e);
        int g = run(stream, cut + n2, got, 4);
        CHECK(g == 1 && got[0].event.channel == 4, "truncated frame then a valid one");
    }

    /* A CRC-valid frame with an unknown type is dropped. */
    {
        uint8_t f[6];
        f[0] = LP_SOF; f[1] = LP_VERSION; f[2] = 0x7F; f[3] = 0;
        uint16_t crc = lp_crc16(&f[1], 3);
        f[4] = (uint8_t)crc; f[5] = (uint8_t)(crc >> 8);
        int g = run(f, sizeof f, got, 4);
        CHECK(g == 0, "unknown type dropped");
    }

    printf("%d/%d link_proto checks passed\n", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
