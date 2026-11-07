/* checkpoint_link: the C5 side of the C5<->bridge UART link (link_proto).
 *
 * A single priority-1 task owns the UART. It announces itself (HELLO), sends
 * STATS (the channels' levels and health) periodically and an EVENT_PASS for
 * every detected pass, and serves the bridge (a C3 node, or the C6 master for its
 * own gate): applies CONFIG_SET / answers CONFIG_REQ and runs a COMMAND
 * (a calibration like the console's B/F, or a reboot), as nobody opens the
 * C5's USB console in production. Pass events reach it through a small queue
 * (checkpoint_link_report_pass, called from the detector task), so every UART
 * write happens from this one task and never races. */
#include "checkpoint_link.h"
#include "sdkconfig.h"
#include <stdio.h>

#if CONFIG_CHECKPOINT_LINK_ENABLE

#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define LINK_UART       ((uart_port_t)CONFIG_CHECKPOINT_LINK_UART_NUM)
#define LINK_PRIORITY   1u      /* below every receiver task, like the detector */
#define LINK_STACK      3072u
#define LINK_RX_BUF     256u
#define LINK_EVQ_DEPTH  4u
#define LINK_READ_MS    100u

static QueueHandle_t s_evq;     /* pd_event_t from the detector task */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void send_frame(const uint8_t *buf, size_t n) {
    if (n) uart_write_bytes(LINK_UART, (const char *)buf, n);
}

static void send_config_state(void) {
    lp_config_t c;
    checkpoint_link_get_config(&c);
    uint8_t buf[LP_MAX_FRAME];
    send_frame(buf, lp_encode_config(buf, sizeof buf, LP_MSG_CONFIG_STATE, &c));
}

static void send_stats(void) {
    lp_stats_t s;
    checkpoint_link_get_stats(&s);
    uint8_t buf[LP_MAX_FRAME];
    send_frame(buf, lp_encode_stats(buf, sizeof buf, &s));
}

static void handle_msg(const lp_msg_t *m) {
    switch (m->type) {
    case LP_MSG_CONFIG_SET:
        (void)checkpoint_link_apply_config(&m->config);
        send_config_state();    /* echo the validated/clamped result */
        break;
    case LP_MSG_CONFIG_REQ:
    case LP_MSG_HELLO:          /* the C3 just connected: sync it */
        send_config_state();
        break;
    case LP_MSG_COMMAND:        /* calibrate or reboot: the bridge's web confirmed it */
        checkpoint_link_command(m->command);
        break;
    default:
        break;                  /* C5 does not consume STATE/EVENT/STATS */
    }
}

static void link_task(void *arg) {
    (void)arg;
    lp_decoder_t dec;
    lp_decoder_init(&dec);

    uint8_t hello[LP_MAX_FRAME];
    send_frame(hello, lp_encode_hello(hello, sizeof hello, LP_ROLE_C5));

    uint32_t last_stats = now_ms();
    for (;;) {
        uint8_t rx[64];
        const int n = uart_read_bytes(LINK_UART, rx, sizeof rx, pdMS_TO_TICKS(LINK_READ_MS));
        for (int i = 0; i < n; i++) {
            lp_msg_t m;
            if (lp_decoder_push(&dec, rx[i], &m)) handle_msg(&m);
        }

        pd_event_t e;
        while (xQueueReceive(s_evq, &e, 0) == pdTRUE) {
            const uint32_t age = now_ms() - e.peak_ms;
            const lp_event_t le = { .channel = e.channel,
                                    .peak_db_x10 = (int32_t)(e.peak_db * 10.0f + (e.peak_db >= 0 ? 0.5f : -0.5f)),
                                    .at_ms = e.peak_ms, .age_ms = age > UINT16_MAX ? UINT16_MAX : (uint16_t)age };
            uint8_t buf[LP_MAX_FRAME];
            send_frame(buf, lp_encode_event(buf, sizeof buf, &le));
        }

        const uint32_t now = now_ms();
        if (now - last_stats >= (uint32_t)CONFIG_CHECKPOINT_LINK_STATS_MS) {
            last_stats = now;
            send_stats();
        }
    }
}

void checkpoint_link_start(void) {
    if (s_evq) return;
    s_evq = xQueueCreate(LINK_EVQ_DEPTH, sizeof(pd_event_t));
    if (!s_evq) { printf("CHECKPOINT link disabled: no memory\n"); return; }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_CHECKPOINT_LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(LINK_UART, LINK_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(LINK_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(LINK_UART, CONFIG_CHECKPOINT_LINK_TX_GPIO,
                                          CONFIG_CHECKPOINT_LINK_RX_GPIO,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        printf("CHECKPOINT link disabled: UART %s\n", esp_err_to_name(err));
        if (uart_is_driver_installed(LINK_UART)) uart_driver_delete(LINK_UART);
        vQueueDelete(s_evq);
        s_evq = NULL;
        return;
    }
    if (xTaskCreate(link_task, "cp_link", LINK_STACK, NULL, LINK_PRIORITY, NULL) != pdPASS) {
        printf("CHECKPOINT link disabled: no memory for the task\n");
        uart_driver_delete(LINK_UART);
        vQueueDelete(s_evq);
        s_evq = NULL;
        return;
    }
    printf("CHECKPOINT link=on uart=%d tx=%d rx=%d baud=%d\n", (int)LINK_UART,
           CONFIG_CHECKPOINT_LINK_TX_GPIO, CONFIG_CHECKPOINT_LINK_RX_GPIO,
           CONFIG_CHECKPOINT_LINK_BAUD);
}

void checkpoint_link_report_pass(const pd_event_t *e) {
    if (s_evq) (void)xQueueSend(s_evq, e, 0);   /* non-blocking; drop if full */
}

#else  /* !CONFIG_CHECKPOINT_LINK_ENABLE */

void checkpoint_link_start(void) {}
void checkpoint_link_report_pass(const pd_event_t *e) { (void)e; }

#endif
