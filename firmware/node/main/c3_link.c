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
#define LOG_RING      4096u
#define C5_SILENT_MS  3000u   /* 3 STATS periods without a frame: the C5 is absent */
#define CONFIG_POLL_MS 1000u  /* CONFIG_REQ period: a console K edit on the C5 echoes nothing */

static const char *TAG = "c3_link";
static QueueHandle_t s_outq;        /* pre-encoded frames to send to the C5 */
static void (*s_on_pass)(const lp_event_t *);
static void (*s_on_stats)(const lp_stats_t *);
static volatile uint32_t s_last_c5_rx;   /* last valid frame from the C5 (ms), 0 = never;
                                          * written by the link task, read by the web */
static uint32_t s_link_start;            /* a C5 that never spoke is silent since then */
static int32_t s_c5_rearm_x10 = -1;      /* its last CONFIG_STATE's re-arm drop; link task only */
/* Relay mode: the master's last CONFIG for this gate (written by the ESP-NOW task, read by the
 * link task), pushed again when the C5 says HELLO. */
static portMUX_TYPE s_master_mux = portMUX_INITIALIZER_UNLOCKED;
static lp_calib_t s_calib;                 /* the C5's passage-calibration state, for the degraded web */
static uint32_t s_calib_ms;                /* when it came, 0 = none */
static portMUX_TYPE s_calib_mux = portMUX_INITIALIZER_UNLOCKED;
static lp_config_t s_master_cfg;
static bool s_has_master_cfg;

typedef struct { uint8_t buf[LP_MAX_FRAME]; uint8_t len; } frame_t;

static void enqueue(const uint8_t *b, size_t n);
static uint32_t now_ms(void);

static char s_log[LOG_RING];
static lp_logring_t s_ring = { .buf = s_log, .cap = LOG_RING };
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_debug;
static volatile bool s_leased;          /* debug was switched on by the master: it expires */
static volatile uint32_t s_lease_until;

void c3_link_debug_set(bool on) {
    uint8_t b[LP_MAX_FRAME];
    portENTER_CRITICAL(&s_log_mux);
    if (on && !s_debug) lp_logring_reset(&s_ring);   /* a fresh log: the page restarts from 0 */
    s_debug = on;
    s_leased = false;                                /* the web's own switch has no lease */
    portEXIT_CRITICAL(&s_log_mux);
    enqueue(b, lp_encode_debug(b, sizeof b, on));
}

void c3_link_debug_lease(uint32_t ms) {
    s_lease_until = now_ms() + ms;
    s_leased = true;
}

bool c3_link_debug(void) { return s_debug; }

static void log_store(const lp_text_t *t) {
    if (!s_debug) return;
    portENTER_CRITICAL(&s_log_mux);
    lp_logring_put(&s_ring, t);
    portEXIT_CRITICAL(&s_log_mux);
}

size_t c3_link_log_read(uint32_t since, char *out, size_t max, uint32_t *next, bool *lost) {
    portENTER_CRITICAL(&s_log_mux);
    const size_t n = lp_logring_read(&s_ring, since, out, max, next, lost);
    portEXIT_CRITICAL(&s_log_mux);
    return n;
}

void c3_link_send_keys(const char *keys, size_t n) {
    if (!s_debug) return;
    uint8_t b[LP_MAX_FRAME];
    enqueue(b, lp_encode_key(b, sizeof b, keys, n));
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

int32_t c3_link_c5_rearm_x10(void) { return s_c5_rearm_x10; }

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

void c3_link_set_master_config(const lp_config_t *c) {
    portENTER_CRITICAL(&s_master_mux);
    s_master_cfg = *c;
    s_has_master_cfg = true;
    portEXIT_CRITICAL(&s_master_mux);
    c3_link_push_config(c);
}

bool c3_link_calib_get(lp_calib_t *out) {
    portENTER_CRITICAL(&s_calib_mux);
    *out = s_calib;
    const uint32_t at = s_calib_ms;
    portEXIT_CRITICAL(&s_calib_mux);
    return at && out->phase != LP_CALIB_NONE && now_ms() - at <= 4000u;
}

void c3_link_calib_start(uint8_t step) {
    if (step == 0) {
        portENTER_CRITICAL(&s_calib_mux);
        s_calib_ms = 0;                                      /* ended: the page stops showing it */
        portEXIT_CRITICAL(&s_calib_mux);
    }
    uint8_t b[LP_MAX_FRAME];
    enqueue(b, lp_encode_calib_start(b, sizeof b, step));   /* 0 (nothing sent) over step 2 */
}

void c3_link_send_gain(uint8_t gain) {
    uint8_t b[LP_MAX_FRAME];
    enqueue(b, lp_encode_gain(b, sizeof b, gain));   /* 0 (nothing sent) outside the range */
}

void c3_link_send_command(lp_cmd_t cmd) {
    uint8_t b[LP_MAX_FRAME];
    enqueue(b, lp_encode_command(b, sizeof b, cmd));   /* 0 (nothing sent) for an unknown cmd */
}

static void handle_msg(const lp_msg_t *m) {
    uint8_t b[LP_MAX_FRAME];
    switch (m->type) {
    case LP_MSG_LOG:        log_store(&m->data); break;
    case LP_MSG_CALIB_STATE:
        portENTER_CRITICAL(&s_calib_mux);
        s_calib = m->calib;
        s_calib_ms = now_ms() | 1u;
        portEXIT_CRITICAL(&s_calib_mux);
        c3_fleet_report_calib(&m->calib);
        break;
    case LP_MSG_EVENT_PASS: if (s_on_pass) s_on_pass(&m->event); break;
    case LP_MSG_STATS:
        if (s_on_stats) s_on_stats(&m->stats);
        /* The outgoing queue may have dropped our DEBUG frame, or the C5 rebooted since: say it
         * again with every STATS while debug is on (the C5 ignores a repeat). */
        if (s_debug) enqueue(b, lp_encode_debug(b, sizeof b, true));
        break;
    /* The C5's real config (answer to CONFIG_REQ / our HELLO, echo of a
     * CONFIG_SET): the degraded web's form and VTX guard follow it. RAM only
     * (the C5 keeps its own prefs) and never pushed back to the C5. */
    case LP_MSG_CONFIG_STATE:
        c3_store_set_config(&m->config, false);
        s_c5_rearm_x10 = m->config.rearm_db_x10;   /* relayed to the C6 (NODE_STATUS) */
        break;
    /* The C5 (re)booted after our HELLO went out, or was swapped. A relay gives it the master's
     * last CONFIG again: one sent while it booted was lost, as its UART comes up after its radio.
     * A degraded C3 asks for its config (its page follows the C5's). Either answer is a
     * CONFIG_STATE. */
    case LP_MSG_HELLO: {
        portENTER_CRITICAL(&s_calib_mux);
        s_calib_ms = 0;                                      /* a rebooted C5 has no session */
        portEXIT_CRITICAL(&s_calib_mux);
        lp_config_t c = {0};
        portENTER_CRITICAL(&s_master_mux);
        const bool master = s_has_master_cfg;
        if (master) c = s_master_cfg;
        portEXIT_CRITICAL(&s_master_mux);
        enqueue(b, master ? lp_encode_config(b, sizeof b, LP_MSG_CONFIG_SET, &c)
                          : lp_encode_config_req(b, sizeof b));
        break;
    }
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
    bool silent = false;
    /* Re-polled every second, as the C6 polls its own C5: the drop relayed in NODE_STATUS
     * and the degraded page's form follow a console K edit within about that. */
    uint8_t req[LP_MAX_FRAME];
    const size_t rn = lp_encode_config_req(req, sizeof req);
    uint32_t last_req = now_ms();

    for (;;) {
        uint8_t rx[64];
        const int n = uart_read_bytes(LINK_UART, rx, sizeof rx, pdMS_TO_TICKS(LINK_READ_MS));
        for (int i = 0; i < n; i++) {
            lp_msg_t m;
            if (lp_decoder_push(&dec, rx[i], &m)) {
                if (!s_last_c5_rx || silent) ESP_LOGI(TAG, "C5 heard");
                s_last_c5_rx = now_ms();
                handle_msg(&m);
            }
        }
        frame_t f;
        while (xQueueReceive(s_outq, &f, 0) == pdTRUE)
            uart_write_bytes(LINK_UART, (const char *)f.buf, f.len);

        const uint32_t t = now_ms();
        /* Debug mode the master switched on, unpolled for FP_DEBUG_LEASE_MS: it left, off again. */
        if (s_debug && s_leased && (int32_t)(t - s_lease_until) > 0) {
            ESP_LOGW(TAG, "debug mode lease over: off");
            c3_link_debug_set(false);
        }
        if (rn && t - last_req >= CONFIG_POLL_MS) {
            last_req = t;
            uart_write_bytes(LINK_UART, (const char *)req, rn);
        }

        /* Status LED: red if the C5 link is silent (its STATS stopped, or never came),
         * else the fleet state. The C5 has no LED, so no keepalive is sent. */
        const uint32_t heard = s_last_c5_rx ? s_last_c5_rx : s_link_start;
        const bool now_silent = (uint32_t)(t - heard) > C5_SILENT_MS;
        if (now_silent && !silent)
            ESP_LOGW(TAG, "no frame from the C5 for %u ms: check its wiring and that it runs protocol v%u",
                     C5_SILENT_MS, LP_VERSION);
        silent = now_silent;
        status_led_set_base(silent ? SLED_ERR_RECOV : c3_fleet_led_base(t));
    }
}

void c3_link_start(void) {
    s_link_start = now_ms();
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
