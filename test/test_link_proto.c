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
        .stale_ms = 1000, .volume_pct = 60, .muted = 0,
    };
    return c;
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
                  r->stale_ms == c.stale_ms &&
                  r->volume_pct == c.volume_pct && r->muted == c.muted;
        CHECK(ok, "config_set roundtrip (all fields)");
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

    /* EVENT_PASS and STATS roundtrip. */
    {
        lp_event_t e = { .channel = 5, .peak_db_x10 = 352, .at_ms = 123456 };
        size_t n = lp_encode_event(buf, sizeof buf, &e);
        int g = run(buf, n, got, 4);
        bool ok = g == 1 && got[0].type == LP_MSG_EVENT_PASS &&
                  got[0].event.channel == 5 && got[0].event.peak_db_x10 == 352 &&
                  got[0].event.at_ms == 123456u;
        CHECK(ok, "event_pass roundtrip");

        lp_stats_t s = { .played = 7, .dropped = 1, .muted = 2, .failed = 0,
                         .cn_x10 = LP_CN_UNKNOWN, .channel = 3, .state = 2 };
        n = lp_encode_stats(buf, sizeof buf, &s);
        g = run(buf, n, got, 4);
        ok = g == 1 && got[0].type == LP_MSG_STATS &&
             got[0].stats.played == 7 && got[0].stats.cn_x10 == LP_CN_UNKNOWN &&
             got[0].stats.channel == 3 && got[0].stats.state == 2;
        CHECK(ok, "stats roundtrip (negative cn preserved)");
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
