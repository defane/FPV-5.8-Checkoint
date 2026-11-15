/* The detector's USB serial console. Structure after C5VRX-5 console.c (Twotoz and the C5VRX
 * contributors, https://github.com/Twotoz/C5VRX, 6bc9e4b, GPL-3.0-only): a high-priority reader
 * that handles the download key itself, and a console task for the other keys.
 *
 * The console never calls the engine: T reads it, and every key that changes it (F, B, K ...)
 * posts a request the scan task applies (checkpoint.c). Benches run in the scan task too. */
#include <inttypes.h>
#include <stdio.h>
#include "checkpoint.h"
#include "checkpoint_link.h"
#include "cp_internal.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "hal/usb_serial_jtag_ll.h"

#define USB_STACK        3072u   /* printf of the heartbeat and the whole reboot path (esp_wifi_stop) */
#define USB_PRIORITY     10u
#define CONSOLE_STACK    6144u
#define CONSOLE_PRIORITY 2u
#define KEY_QUEUE        64u
#define HEARTBEAT_US     5000000
#define K_WINDOW_US      5000000  /* K: 5 s per settings key, q closes */

static QueueHandle_t s_keys;
static TaskHandle_t s_usb, s_console;

TaskHandle_t cp_console_task(void) { return s_console; }
TaskHandle_t cp_usb_task(void) { return s_usb; }

static unsigned stack_free(TaskHandle_t t) { return t ? (unsigned)uxTaskGetStackHighWaterMark(t) : 0u; }

static void status(void)
{
    firmware_print();
    printf("STATUS uptime_s=%lld heap_internal=%u heap_largest=%u heap_min=%u heap_ok=%u scan_stack_free=%u "
           "console_stack_free=%u usb_stack_free=%u link_stack_free=%u fw=%s\n",
           esp_timer_get_time() / 1000000, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           /* Lowest free since boot: the boot peak, which a reading now cannot show. */
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), heap_caps_check_integrity_all(false),
           stack_free(cp_scan_task()), stack_free(s_console), stack_free(s_usb), stack_free(checkpoint_link_task()),
           firmware_id());
    sdr_rx_print();
    checkpoint_print();
}

static void help(void)
{
    printf("Detector keys: T status, F recalibrate every LO, B recalibrate + verify the image rejection (VTX off),\n"
           "  K + key checkpoint detector settings and engine benches, ` reboot to USB download (flashing)\n");
}

/* The USB reader runs above every other task of ours: the download key and the heartbeat work
 * however busy the CPU is. It drains the FIFO, handles the download key itself and queues the
 * rest for the console task. */
static void usb_reader_task(void *arg)
{
    (void)arg;
    int64_t beat = 0;
    for (;;) {
        for (unsigned k = 0; k < 64; ++k) {
            uint8_t c;
            if (usb_serial_jtag_ll_read_rxfifo(&c, 1) == 0) break;
            if (c == '`') sdr_rx_reboot_to_download();   /* quiesces the captures; never returns */
            (void)xQueueSend(s_keys, &c, 0);
        }
        /* Heartbeat: shows the board is alive whatever the console does. */
        const int64_t now = esp_timer_get_time();
        if (now - beat > HEARTBEAT_US) {
            beat = now;
            const cp_t *cp = cp_state();
            bool live = false, stalled = false;
            if (cp) cp_health(cp, &live, &stalled);
            sdr_rx_health_t h;
            sdr_rx_get_health(&h);
            printf("HB t_s=%lld stage=%s live=%u rot_hz_x10=%u faults=%" PRIu32 " cal=%u\n", now / 1000000,
                   cp_boot_stage(), live, (unsigned)h.rot_hz_x10, sdr_rx_fault_count(), sdr_rx_calibrating());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void console_task(void *arg)
{
    (void)arg;
    int64_t k_until = 0;
    for (;;) {
        uint8_t c;
        if (xQueueReceive(s_keys, &c, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (k_until && esp_timer_get_time() < k_until) {
            if (c == 'q') { k_until = 0; printf("CHECKPOINT settings closed\n"); continue; }
            if (checkpoint_key((char)c)) { k_until = esp_timer_get_time() + K_WINDOW_US; continue; }
        }
        k_until = 0;
        switch (c) {
        case 'K': checkpoint_help(); k_until = esp_timer_get_time() + K_WINDOW_US; break;
        case 'T': status(); break;
        case 'F': cp_command(LP_CMD_CAL_QUICK, "console"); break;
        case 'B': cp_command(LP_CMD_CAL_FULL, "console"); break;
        case 'h': case '?': help(); break;
        default: break;
        }
    }
}

/* First in app_main: flashing (the download key) and diagnostics stay reachable whatever a
 * later boot step does. */
void checkpoint_console_start(void)
{
    if (s_keys) return;
    s_keys = xQueueCreate(KEY_QUEUE, 1);
    if (!s_keys) { printf("CONSOLE disabled: no memory\n"); return; }
    if (xTaskCreate(usb_reader_task, "usb_reader", USB_STACK, NULL, USB_PRIORITY, &s_usb) != pdPASS)
        printf("CONSOLE usb_reader not started: no memory\n");
    if (xTaskCreate(console_task, "console", CONSOLE_STACK, NULL, CONSOLE_PRIORITY, &s_console) != pdPASS)
        printf("CONSOLE task not started: no memory\n");
}
