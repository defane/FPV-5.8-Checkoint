/* ESP glue for status_led: a WS2812 driver over the RMT bytes encoder (no
 * external component) and a priority-1 task that renders the pure-C core every
 * CONFIG_STATUS_LED_TICK_MS. The firmware pushes state through the thread-safe
 * setters; all are no-ops when CONFIG_STATUS_LED_ENABLE=n. See status_led.h. */
#include "status_led.h"
#include "sdkconfig.h"

#if CONFIG_STATUS_LED_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "status_led";

typedef struct {
    sled_t               core;
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
    volatile sled_base_t base;
    volatile bool        pass_pending;
    volatile int         quality_cn_x10;   /* <0 = bench mode off */
} led_rt_t;

static led_rt_t *s_rt;

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void render(sled_rgb_t c)
{
#if CONFIG_STATUS_LED_RGB_ORDER
    const uint8_t bytes[3] = { c.r, c.g, c.b };
#else
    const uint8_t bytes[3] = { c.g, c.r, c.b };        /* WS2812's usual order */
#endif
    const rmt_transmit_config_t tx = { .loop_count = 0 };
    if (rmt_transmit(s_rt->chan, s_rt->enc, bytes, sizeof bytes, &tx) == ESP_OK)
        (void)rmt_tx_wait_all_done(s_rt->chan, 50);
    /* >=50 us idle before the next frame (next tick is ~50 ms) latches it. */
}

static void led_task(void *arg)
{
    (void)arg;
    /* Boot self-test: red, green, blue at a clearly visible level, so a new
     * board's WS2812 wiring, GPIO and colour order are unmistakable (and not
     * confused with a separate power LED). */
    render((sled_rgb_t){ 100, 0, 0 });  vTaskDelay(pdMS_TO_TICKS(400));
    render((sled_rgb_t){ 0, 100, 0 });  vTaskDelay(pdMS_TO_TICKS(400));
    render((sled_rgb_t){ 0, 0, 100 });  vTaskDelay(pdMS_TO_TICKS(400));

    const TickType_t period = pdMS_TO_TICKS(CONFIG_STATUS_LED_TICK_MS);
    for (;;) {
        sled_set_base(&s_rt->core, s_rt->base);
        if (s_rt->pass_pending) { s_rt->pass_pending = false; sled_pass(&s_rt->core, now_ms()); }
        const int q = s_rt->quality_cn_x10;
        if (q >= 0) {
            int v = q * 255 / 400;             /* 0..40 dB -> 0..255 */
            if (v > 255) v = 255;
            sled_set_quality(&s_rt->core, (uint8_t)v);
        } else {
            sled_clear_quality(&s_rt->core);
        }
        render(sled_tick(&s_rt->core, now_ms()));
        vTaskDelay(period);
    }
}

void status_led_start(void)
{
    if (s_rt) return;
    s_rt = calloc(1, sizeof(*s_rt));
    if (!s_rt) { ESP_LOGE(TAG, "no memory"); return; }
    sled_init(&s_rt->core, now_ms());
    sled_set_brightness(&s_rt->core, (uint8_t)CONFIG_STATUS_LED_BRIGHTNESS);
    s_rt->base = SLED_BOOT;
    s_rt->quality_cn_x10 = -1;

    const rmt_tx_channel_config_t cc = {
        .gpio_num          = CONFIG_STATUS_LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,   /* 10 MHz -> 0.1 us/tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    const rmt_bytes_encoder_config_t ec = {
        .bit0  = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },  /* 0.3/0.9 us */
        .bit1  = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },  /* 0.9/0.3 us */
        .flags = { .msb_first = 1 },
    };
    if (rmt_new_tx_channel(&cc, &s_rt->chan) != ESP_OK ||
        rmt_new_bytes_encoder(&ec, &s_rt->enc) != ESP_OK ||
        rmt_enable(s_rt->chan) != ESP_OK) {
        ESP_LOGE(TAG, "RMT init failed (gpio=%d)", (int)CONFIG_STATUS_LED_GPIO);
        return;                 /* the LED stays dark; the rest of the node runs */
    }
    if (xTaskCreate(led_task, "status_led", CONFIG_STATUS_LED_STACK, NULL, 1, NULL) != pdPASS)
        ESP_LOGE(TAG, "task create failed");
    else
        ESP_LOGI(TAG, "on gpio=%d brightness=%d", (int)CONFIG_STATUS_LED_GPIO,
                 (int)CONFIG_STATUS_LED_BRIGHTNESS);
}

void status_led_set_base(sled_base_t b)        { if (s_rt) s_rt->base = b; }
void status_led_pass(void)                     { if (s_rt) s_rt->pass_pending = true; }
void status_led_set_quality_cn_x10(int cn_x10) { if (s_rt) s_rt->quality_cn_x10 = cn_x10; }

#else  /* !CONFIG_STATUS_LED_ENABLE : no-op so callers need no #ifdef */

void status_led_start(void)                    { }
void status_led_set_base(sled_base_t b)        { (void)b; }
void status_led_pass(void)                     { }
void status_led_set_quality_cn_x10(int cn_x10) { (void)cn_x10; }

#endif
