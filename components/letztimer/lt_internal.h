/* Private to components/letztimer: the detector state shared by letztimer.c (prefs, K keys,
 * link accessors, commands), lt_scan.c (the scan task), lt_bench.c (the console benches) and
 * lt_console.c (the USB console), plus the request mailbox between them.
 *
 * Threads. The scan task (lt_scan.c, priority 1) is the only one that calls the engine
 * (sdr_rx_step, sdr_rx_request_*, sdr_bench_*) and writes the detector (sd). The console
 * (priority 2) and the link task (priority 1) post requests under lt_mux; the scan task applies
 * them at its next hop. Readers on other tasks only read 32-bit fields. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link_proto.h"
#include "pass_calib.h"
#include "pass_detector.h"
#include "scan_detector.h"
#include "sdr_rx.h"

#define LT_SCAN_STACK    6144u      /* bytes */
#define LT_SCAN_PRIORITY 1u
#define LT_YIELD_US      100000     /* every scan-task loop blocks one tick this often */
#define LT_TRACE_US      200000     /* K l: 5 lines a second */
#define LT_STALL_MS      1000u      /* no rotation this long, outside a calibration or bench: stalled;
                                     * a channel without a reading this long reads unknown in STATS */
#define LT_GAIN_MIN      10u
#define LT_GAIN_STEP     2u

/* The passage calibration's session (lt_calib.c): posted by the link, run by the scan task. */
typedef struct {
    volatile uint8_t req;        /* posted under lt_mux: 1 step 1, 2 step 2, 3 end the session; 0 none */
    volatile uint8_t req_ch;     /* the channel posted with step 1: 0..7, or LP_CALIB_ANY */
    uint8_t want_ch;             /* the session's: 0..7 measured as asked, LP_CALIB_ANY the one standing highest */
    lp_calib_t st;               /* what the link reports, under lt_mux */
    bool active;                 /* a session exists */
    uint8_t measuring;           /* 1 or 2 while a measurement runs, else 0 */
    pc_hist_t *hist;             /* heap, only while measuring */
    int64_t end_us;              /* the running measurement's end */
    int64_t idle_us;             /* the session ends then if nothing is asked */
    pc_stats_t gate;             /* step 1's, kept for the solver */
} lt_calib_t;

typedef struct {
    scan_detector_t sd;
    pd_config_t wanted; volatile bool wanted_dirty;       /* console/link copy, applied by the scan task */
    float wanted_rearm_db;                                 /* the relative re-arm's drop beside it, dB (0 = off) */
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
    volatile uint32_t last_reading_ms[SDR_RX_CHANNELS];    /* per channel, the same; reset with last_rotation_ms */
    int16_t image_floor_qdb[SDR_RX_CHANNELS];              /* K x floor run */
    uint8_t image_floor_gain;                              /* 0 = no floor run yet */
    int64_t last_trace_us;
    uint16_t trace_sat[SDR_RX_MAX_HOPS];                   /* K l: saturated captures per hop since the last line */
    uint32_t held_at_trace[SDR_RX_CHANNELS];               /* sd.held as the last K l line printed */
    uint32_t held_at_pass[SDR_RX_CHANNELS];                /* sd.held as the channel's last pass printed */
    lt_calib_t calib;                                      /* the passage calibration session */
    uint32_t readings, rotations, passes;
    bool has_last; pd_event_t last;
} lt_t;

extern portMUX_TYPE lt_mux;   /* the mailbox fields of lt_t (wanted, wanted_rearm_db, gain, recal_req, bench_req) */

typedef struct {
    int64_t last_us;
    uint32_t count;           /* yields so far: benches drop the rotation that contains one */
} lt_yield_t;

/* The task watchdog has the scan task (lt_scan.c, after a successful sdr_rx_init). */
extern bool lt_wdt_subscribed;

/* Every loop of the scan task calls this: scan, every bench loop, the K S recording. On a
 * single core, a task that never blocks starves IDLE (whose watchdog check would trip) and
 * the link. */
static inline void lt_yield(lt_yield_t *y)
{
    const int64_t now = esp_timer_get_time();
    if (now - y->last_us < LT_YIELD_US) return;
    vTaskDelay(1);                           /* IDLE runs (TWDT IDLE check), lt_link gets the CPU */
    if (lt_wdt_subscribed) (void)esp_task_wdt_reset();
    y->last_us = esp_timer_get_time();
    y->count++;
}

static inline uint32_t lt_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* letztimer.c */
lt_t *lt_state(void);                        /* NULL before letztimer_start */
const char *lt_boot_stage(void);
/* Live (levels real) and stalled (no rotation for LT_STALL_MS outside a calibration or a
 * bench), computed now. */
void lt_health(const lt_t *cp, bool *live, bool *stalled);
/* The scan task, whenever it sets last_rotation_ms by hand (live, after a calibration or a
 * bench): every channel counts as just read. */
void lt_mark_read(lt_t *cp, uint32_t now_ms);
/* A link COMMAND or the console's F/B: posts it to the scan task, or prints the refusal. */
void lt_command(uint8_t lp_cmd, const char *from);

/* lt_scan.c */
bool lt_scan_reserve(void);                  /* the scan task's stack and TCB, at boot */
bool lt_scan_start(lt_t *cp, int64_t boot_t0_us);
TaskHandle_t lt_scan_task(void);
void lt_on_pass(lt_t *cp, const pd_event_t *e);
/* [scan] One hop's readings into sd: sd_feed_hop (CONFIG_LETZTIMER_PAIR_GUARD) or sd_feed each.
 * A bit per reading that completed a pass (ev[i]); *held a bit per reading the guard held. */
unsigned lt_feed_hop(scan_detector_t *sd, const sdr_hop_t *h, pd_event_t ev[SDR_RX_PER_HOP], unsigned *held);

/* lt_calib.c: the passage calibration (docs/detector.md). The link posts, the scan task runs it: it
 * samples each channel's smoothed rise once per rotation for LP_CALIB_SECS s, never blinds the gate. */
void lt_calib_request(lt_t *cp, uint8_t step, uint8_t channel);   /* [any] 0 ends the session, 1 passage, 2 away; channel: step 1's */
void lt_calib_apply(lt_t *cp, int64_t now_us);              /* [scan] between two hops: take the request, time out */
void lt_calib_capture(lt_t *cp, bool saturated);            /* [scan] one capture of a measurement */
void lt_calib_rotation(lt_t *cp, int64_t now_us);           /* [scan] each rotation: sample, end the window */
bool lt_calib_get(lt_t *cp, lp_calib_t *out);               /* [link] false without a session */

/* lt_console.c */
TaskHandle_t lt_console_task(void);
TaskHandle_t lt_usb_task(void);
void lt_console_inject(const char *keys, size_t n);   /* the link's KEY frames, as typed keys */

/* lt_nvs.c: blobs in NVS namespace "letztimer". */
bool lt_blob_load(const char *key, void *data, size_t size);
bool lt_blob_store(const char *key, const void *data, size_t size);
