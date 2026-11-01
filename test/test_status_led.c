/* Host test for the status_led core. Build:
 *   cc -O2 -std=c11 -Wall -Wextra -Werror -I../components/status_led/include \
 *     test_status_led.c ../components/status_led/status_led.c -o /tmp/t && /tmp/t
 */
#include "status_led.h"
#include <stdio.h>

static int fails;
#define CHECK(name, cond) do { \
    if (cond) { printf("ok: %s\n", (name)); } \
    else { printf("FAIL: %s\n", (name)); fails++; } } while (0)

static int on(sled_rgb_t c) { return (c.r | c.g | c.b) != 0; }

int main(void)
{
    sled_t s;
    sled_init(&s, 1000u);
    sled_set_brightness(&s, 255u);      /* full scale makes asserts simple */

    /* BOOT: yellow, square blink. t0 = 1000, half = 250. */
    sled_set_base(&s, SLED_BOOT);
    sled_rgb_t c = sled_tick(&s, 1000u);
    CHECK("boot is yellow (R+G, no B)", c.r > 0 && c.g > 0 && c.b == 0);
    CHECK("boot off at +250ms",  !on(sled_tick(&s, 1250u)));
    CHECK("boot on  at +500ms",   on(sled_tick(&s, 1500u)));

    /* ERR_FATAL: solid red, no blink. */
    sled_set_base(&s, SLED_ERR_FATAL);
    c = sled_tick(&s, 2000u);
    CHECK("fatal is red",        c.r > 0 && c.g == 0 && c.b == 0);
    CHECK("fatal stays on",      on(sled_tick(&s, 2250u)));

    /* ERR_RECOV: red fast blink, half = 100. */
    sled_set_base(&s, SLED_ERR_RECOV);
    c = sled_tick(&s, 1000u);
    CHECK("recov red on at phase 0", c.r > 0 && c.g == 0 && c.b == 0);
    CHECK("recov off at +100ms", !on(sled_tick(&s, 1100u)));

    /* HEALTHY: green, breathing brightens from the trough. */
    sled_set_base(&s, SLED_HEALTHY);
    sled_rgb_t lo = sled_tick(&s, 1000u);        /* phase 0  -> trough */
    sled_rgb_t hi = sled_tick(&s, 2500u);        /* half     -> peak   */
    CHECK("healthy is green",    lo.g > 0 && lo.r == 0 && lo.b == 0);
    CHECK("breathing brightens", hi.g > lo.g);

    /* PASS: white flash overrides base, then clears back to green. */
    sled_pass(&s, 3000u);
    c = sled_tick(&s, 3000u);
    CHECK("pass is white",       c.r > 0 && c.g > 0 && c.b > 0);
    CHECK("pass holds at +100ms", on(sled_tick(&s, 3100u)));
    c = sled_tick(&s, 3130u);
    CHECK("pass cleared -> green", c.g > 0 && c.r == 0 && c.b == 0);

    /* ERR_FATAL beats a pass flash. */
    sled_set_base(&s, SLED_ERR_FATAL);
    sled_pass(&s, 4000u);
    c = sled_tick(&s, 4000u);
    CHECK("fatal beats pass (red)", c.r > 0 && c.g == 0 && c.b == 0);

    /* HEALTHY + quality: brightness tracks C/N (bench mode), still green. */
    sled_set_base(&s, SLED_HEALTHY);
    sled_set_quality(&s, 0u);
    sled_rgb_t qlo = sled_tick(&s, 9000u);
    sled_set_quality(&s, 255u);
    sled_rgb_t qhi = sled_tick(&s, 9000u);
    CHECK("quality high brighter than low", qhi.g > qlo.g && qhi.r == 0 && qhi.b == 0);
    sled_clear_quality(&s);

    /* Master brightness scales to off. */
    sled_set_base(&s, SLED_ERR_FATAL);
    sled_set_brightness(&s, 0u);
    CHECK("brightness 0 -> off", !on(sled_tick(&s, 5000u)));

    if (fails) { printf("%d test(s) FAILED\n", fails); return 1; }
    printf("all status_led tests passed\n");
    return 0;
}
