#include "c6_link.h"
#include "c6_hub.h"
#include "c6_store.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link_proto.h"
#include "sdkconfig.h"

#define LINK_UART     ((uart_port_t)CONFIG_C6_UART_NUM)
#define LINK_RX_BUF   256u
#define LINK_READ_MS  100u

static const char *TAG = "c6_link";

static void on_pass(const lp_event_t *e) {
    c6_hub_local_pass(e->channel, e->peak_db_x10, e->at_ms);   /* gate 0, timed + sounded by the hub */
    ESP_LOGI(TAG, "local PASS ch=%u peak=%.1f dB", e->channel, e->peak_db_x10 / 10.0f);
}

static void handle_msg(const lp_msg_t *m) {
    switch (m->type) {
    case LP_MSG_EVENT_PASS: on_pass(&m->event); break;
    case LP_MSG_STATS:      c6_store_set_local_stats(&m->stats); break;
    default: break;
    }
}

static void link_task(void *arg) {
    (void)arg;
    lp_decoder_t dec;
    lp_decoder_init(&dec);
    uint8_t hello[LP_MAX_FRAME];
    size_t hn = lp_encode_hello(hello, sizeof hello, LP_ROLE_C3);  /* "I am the UART master" */
    if (hn) uart_write_bytes(LINK_UART, (const char *)hello, hn);

    uint32_t last_req = 0;
    for (;;) {
        uint8_t rx[64];
        const int n = uart_read_bytes(LINK_UART, rx, sizeof rx, pdMS_TO_TICKS(LINK_READ_MS));
        for (int i = 0; i < n; i++) {
            lp_msg_t m;
            if (lp_decoder_push(&dec, rx[i], &m)) handle_msg(&m);
        }
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - last_req >= 1000u) {
            last_req = now;
            uint8_t req[LP_MAX_FRAME];
            size_t r = lp_encode_config_req(req, sizeof req);
            if (r) uart_write_bytes(LINK_UART, (const char *)req, r);
        }
    }
}

void c6_link_start(void) {
    const uart_config_t cfg = {
        .baud_rate = CONFIG_C6_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, LINK_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, CONFIG_C6_UART_TX_GPIO, CONFIG_C6_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(link_task, "c6_link", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "local C5 link uart=%d tx=%d rx=%d baud=%d", (int)LINK_UART,
             CONFIG_C6_UART_TX_GPIO, CONFIG_C6_UART_RX_GPIO, CONFIG_C6_UART_BAUD);
}
