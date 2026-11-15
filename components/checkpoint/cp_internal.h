/* Private to components/checkpoint: the detector state shared by checkpoint.c (prefs, K keys,
 * link accessors, commands), cp_scan.c (the scan task), cp_bench.c (the console benches) and
 * cp_console.c (the USB console), plus the request mailbox between them.
 *
 * Threads. The scan task (cp_scan.c, priority 1) is the only one that calls the engine
 * (sdr_rx_step, sdr_rx_request_*, sdr_bench_*) and writes the detector (sd). The console
 * (priority 2) and the link task (priority 1) post requests under cp_mux; the scan task applies
 * them at its next hop. Readers on other tasks only read 32-bit fields. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pass_detector.h"
#include "scan_detector.h"
#include "sdr_rx.h"

#define CP_SCAN_STACK    6144u      /* bytes */
#define CP_SCAN_PRIORITY 1u
#define CP_YIELD_US      100000     /* every scan-task loop blocks one tick this often */
#define CP_TRACE_US      200000     /* K l: 5 lines a second */
#define CP_STALL_MS      1000u      /* no rotation this long, outside a calibration or bench: stalled */
#define CP_GAIN_MIN      10u
#define CP_GAIN_STEP     2u

typedef struct {
    scan_detector_t sd;
    pd_config_t wanted; volatile bool wanted_dirty;       /* console/link copy, applied by the scan task */
    uint8_t gain;                                          /* saved; applied through sdr_rx_request_gain */
    volatile bool gain_dirty;                              /* gain posted, not yet applied */
    volatile bool trace, live, learning, bench_running;    /* live: the engine is up and no bench runs */
    volatile bool engine_failed;                           /* sdr_rx_init failed: CAL_NEEDED + a fault */
    volatile uint8_t bench_req;                            /* the bench key posted by the console, 0 = none */
    volatile uint8_t bench_key;                            /* the bench running (bench_running) */
    volatile int8_t recal_req;                             /* a posted sdr_recal_t, -1 = none */
    const char *recal_from;                                /* who posted it (refusal line) */
    volatile bool scan_stop;                               /* a second K S while recording */
    volatile bool relearn_pending;                         /* set by an accepted command/antenna/gain change */
    volatile uint32_t last_rotation_ms;                    /* scan task writes (32-bit: no torn read) */
    int16_t image_floor_qdb[SDR_RX_CHANNELS];              /* K x floor run */
    uint8_t image_floor_gain;                              /* 0 = no floor run yet */
    int64_t last_trace_us;
    uint32_t readings, rotations, passes;
    bool has_last; pd_event_t last;
} cp_t;

extern portMUX_TYPE cp_mux;   /* the mailbox fields of cp_t (wanted, gain, recal_req, bench_req) */

typedef struct {
    int64_t last_us;
    uint32_t count;           /* yields so far: benches drop the rotation that contains one */
} cp_yield_t;

/* The task watchdog has the scan task (cp_scan.c, after a successful sdr_rx_init). */
extern bool cp_wdt_subscribed;

/* Every loop of the scan task calls this: scan, every bench loop, the K S recording. On a
 * single core, a task that never blocks starves IDLE (whose watchdog check would trip) and
 * the link. */
static inline void cp_yield(cp_yield_t *y)
{
    const int64_t now = esp_timer_get_time();
    if (now - y->last_us < CP_YIELD_US) return;
    vTaskDelay(1);                           /* IDLE runs (TWDT IDLE check), cp_link gets the CPU */
    if (cp_wdt_subscribed) (void)esp_task_wdt_reset();
    y->last_us = esp_timer_get_time();
    y->count++;
}

static inline uint32_t cp_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* checkpoint.c */
cp_t *cp_state(void);                        /* NULL before checkpoint_start */
const char *cp_boot_stage(void);
/* Live (levels real) and stalled (no rotation for CP_STALL_MS outside a calibration or a
 * bench), computed now. */
void cp_health(const cp_t *cp, bool *live, bool *stalled);
/* A link COMMAND or the console's F/B: posts it to the scan task, or prints the refusal. */
void cp_command(uint8_t lp_cmd, const char *from);

/* cp_scan.c */
bool cp_scan_reserve(void);                  /* the scan task's stack and TCB, at boot */
bool cp_scan_start(cp_t *cp, int64_t boot_t0_us);
TaskHandle_t cp_scan_task(void);
void cp_on_pass(cp_t *cp, const pd_event_t *e);

/* cp_console.c */
TaskHandle_t cp_console_task(void);
TaskHandle_t cp_usb_task(void);

/* cp_nvs.c: blobs in NVS namespace "checkpoint". */
bool cp_blob_load(const char *key, void *data, size_t size);
bool cp_blob_store(const char *key, const void *data, size_t size);
