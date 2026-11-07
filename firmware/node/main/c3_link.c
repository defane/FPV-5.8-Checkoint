#include "c3_link.h"
#include "c3_fleet.h"
#include "c3_store.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "status_led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

#define LINK_UART     ((uart_port_t)CONFIG_C3_UART_NUM)
#define LINK_RX_BUF   256u
#define LINK_READ_MS  100u
#define OUTQ_DEPTH    4u
#define C5_SILENT_MS  3000u   /* 3 STATS periods without a frame: the C5 is absent */

static const char *TAG = "c3_link";
static QueueHandle_t s_outq;        /* pre-encoded frames to send to the C5 */
static void (*s_on_pass)(const lp_event_t *);
static void (*s_on_stats)(const lp_stats_t *);
static volatile uint32_t s_last_c5_rx;   /* last valid frame from the C5 (ms), 0 = never;
                                          * written by the link task, read by the web */

typedef struct { uint8_t buf[LP_MAX_FRAME]; uint8_t len; } frame_t;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool c3_link_c5_alive(void) {
    const uint32_t last = s_last_c5_rx;
    return last != 0u && (uint32_t)(now_ms() - last) <= C5_SILENT_MS;
}

static void enqueue(const uint8_t *b, size_t n) {
    if (!n || !s_outq) return;
    frame_t f;
    memcpy(f.buf, b, n);
    f.len = (uint8_t)n;
    (void)xQueueSend(s_outq, &f, 0);
}

void c3_link_set_handlers(void (*on_pass)(const lp_event_t *),
                          void (*on_stats)(const lp_stats_t *)) {
    s_on_pass = on_pass;
    s_on_stats = on_stats;
}

void c3_link_push_config(const lp_config_t *c) {
    uint8_t b[LP_MAX_FRAME];
    enqueue(b, lp_encode_config(b, sizeof b, LP_MSG_CONFIG_SET, c));
}

void c3_link_send_command(lp_cmd_t cmd) {
    uint8_t b[LP_MAX_FRAME];
    enqueue(b, lp_encode_command(b, sizeof b, cmd));   /* 0 (nothing sent) for an unknown cmd */
}

static void handle_msg(const lp_msg_t *m) {
    uint8_t b[LP_MAX_FRAME];
    switch (m->type) {
    case LP_MSG_EVENT_PASS: if (s_on_pass) s_on_pass(&m->event); break;
    case LP_MSG_STATS:      if (s_on_stats) s_on_stats(&m->stats); break;
    /* The C5's real config (answer to CONFIG_REQ / our HELLO, echo of a
     * CONFIG_SET): the degraded web's form and VTX guard follow it. RAM only
     * (the C5 keeps its own prefs) and never pushed back to the C5. */
    case LP_MSG_CONFIG_STATE: c3_store_set_config(&m->config, false); break;
    /* The C5 (re)booted after our HELLO went out: ask it for its config. */
    case LP_MSG_HELLO:        enqueue(b, lp_encode_config_req(b, sizeof b)); break;
    default: break;
    }
}

static void link_task(void *arg) {
    (void)arg;
    lp_decoder_t dec;
    lp_decoder_init(&dec);
    uint8_t hello[LP_MAX_FRAME];
    size_t hn = lp_encode_hello(hello, sizeof hello, LP_ROLE_C3);
    if (hn) uart_write_bytes(LINK_UART, (const char *)hello, hn);

    for (;;) {
        uint8_t rx[64];
        const int n = uart_read_bytes(LINK_UART, rx, sizeof rx, pdMS_TO_TICKS(LINK_READ_MS));
        for (int i = 0; i < n; i++) {
            lp_msg_t m;
            if (lp_decoder_push(&dec, rx[i], &m)) {
                s_last_c5_rx = now_ms();
                handle_msg(&m);
            }
        }
        frame_t f;
        while (xQueueReceive(s_outq, &f, 0) == pdTRUE)
            uart_write_bytes(LINK_UART, (const char *)f.buf, f.len);

        /* Status LED: red if the C5 link went silent (its STATS stopped),
         * else the fleet state. The C5 has no LED, so no keepalive is sent. */
        const uint32_t t = now_ms();
        status_led_set_base((s_last_c5_rx != 0u && (uint32_t)(t - s_last_c5_rx) > C5_SILENT_MS)
                            ? SLED_ERR_RECOV : c3_fleet_led_base(t));
    }
}

void c3_link_start(void) {
    s_outq = xQueueCreate(OUTQ_DEPTH, sizeof(frame_t));
    const uart_config_t cfg = {
        .baud_rate = CONFIG_C3_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, LINK_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, CONFIG_C3_UART_TX_GPIO, CONFIG_C3_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(link_task, "c3_link", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "C5 link uart=%d tx=%d rx=%d baud=%d", (int)LINK_UART,
             CONFIG_C3_UART_TX_GPIO, CONFIG_C3_UART_RX_GPIO, CONFIG_C3_UART_BAUD);
}
