#include "c3_link.h"
#include "c3_store.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define LINK_UART     ((uart_port_t)CONFIG_C3_UART_NUM)
#define LINK_RX_BUF   256u
#define LINK_READ_MS  100u
#define LINK_OUTQ     2u

static const char *TAG = "c3_link";
static QueueHandle_t s_outq;   /* lp_config_t to send as CONFIG_SET */

static void send_frame(const uint8_t *buf, size_t n) {
    if (n) uart_write_bytes(LINK_UART, (const char *)buf, n);
}

static void send_config_set(const lp_config_t *c) {
    uint8_t buf[LP_MAX_FRAME];
    send_frame(buf, lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_SET, c));
}

static void send_simple(size_t (*enc)(uint8_t *, size_t)) {
    uint8_t buf[LP_MAX_FRAME];
    send_frame(buf, enc(buf, sizeof buf));
}

/* The C5 (re)started or we are starting: enforce our config if we have one,
 * otherwise ask for the C5's so a fresh C3 adopts it. */
static void resync(void) {
    lp_config_t c;
    if (c3_store_get_config(&c)) send_config_set(&c);
    else send_simple(lp_encode_config_req);
}

static void handle_msg(const lp_msg_t *m) {
    switch (m->type) {
    case LP_MSG_CONFIG_STATE: {
        lp_config_t cur;
        if (!c3_store_get_config(&cur)) c3_store_set_config(&m->config, true); /* adopt */
        break;
    }
    case LP_MSG_EVENT_PASS:
        c3_store_add_event(&m->event);
        break;
    case LP_MSG_STATS:
        c3_store_set_stats(&m->stats);
        break;
    case LP_MSG_HELLO:          /* C5 announced itself: it just booted */
        resync();
        break;
    default:
        break;
    }
}

static void link_task(void *arg) {
    (void)arg;
    lp_decoder_t dec;
    lp_decoder_init(&dec);

    uint8_t buf[LP_MAX_FRAME];
    send_frame(buf, lp_encode_hello(buf, sizeof buf, LP_ROLE_C3));
    resync();

    for (;;) {
        uint8_t rx[64];
        const int n = uart_read_bytes(LINK_UART, rx, sizeof rx, pdMS_TO_TICKS(LINK_READ_MS));
        for (int i = 0; i < n; i++) {
            lp_msg_t m;
            if (lp_decoder_push(&dec, rx[i], &m)) handle_msg(&m);
        }
        lp_config_t out;
        while (xQueueReceive(s_outq, &out, 0) == pdTRUE) send_config_set(&out);
    }
}

void c3_link_start(void) {
    if (s_outq) return;
    s_outq = xQueueCreate(LINK_OUTQ, sizeof(lp_config_t));

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
    ESP_LOGI(TAG, "link uart=%d tx=%d rx=%d baud=%d", (int)LINK_UART,
             CONFIG_C3_UART_TX_GPIO, CONFIG_C3_UART_RX_GPIO, CONFIG_C3_UART_BAUD);
}

void c3_link_push_config(const lp_config_t *c) {
    if (s_outq) (void)xQueueSend(s_outq, c, 0);
}
