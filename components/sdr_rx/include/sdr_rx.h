/* sdr_rx: the detector's receive engine (ESP32-C5). Every Raceband channel R1..R8 measured
 * ~500 times a second: one LO for two channels (5677/5751/5825/5899 MHz), a 256-sample
 * snapshot of the PHY's I/Q dump at 80 MS/s per LO, a 4 x 64-point integer Welch PSD with
 * moment-based I/Q correction, and the band power within +-5 MHz of each channel.
 * Ported from widscan58 (CapnBry, a9fff97), whose radio layer derives from esp-sdr (ESPARGOS,
 * a8eaac1); the dump engine itself was first decoded by C5VRX-1 (Twotoz and the C5VRX
 * contributors). See README "Attribution".
 *
 * Research provenance. The PHY I/Q dump engine was first decoded in C5VRX-1: librftest
 * adctrig, registers 0x600a9004/08/18, bank 0x40830000, 10-bit I/Q words (upstream
 * legacy/c5vrx1/research/adc-dump-format.md). C5VRX-3/5 drove it continuously
 * (enable_continuous_modem, upstream main/rf.c:119-147). This route is different: one-shot
 * snapshots as esp-sdr/widscan58 trigger them. Source: raw source 15, after the PHY
 * ADC-rate/RX-filter stage. Rate: 80 MS/s. Size: 256 words per capture (3.2 us). Ownership:
 * CPU-polled, no DMA, no ring, no DAC. Word format: I bits 0-9, Q bits 10-19, signed.
 *
 * Threads: functions marked [scan] are for the one task that owns the engine (the checkpoint
 * scan task); [any] may be called from any task. GPL-3.0-only. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdr_types.h"

typedef enum { SDR_RECAL_QUICK = 0, SDR_RECAL_FULL = 1 } sdr_recal_t;

typedef struct {
    int64_t t_us;        /* middle of the capture, esp_timer_get_time() clock */
    int16_t level_qdb;   /* power within +-5 MHz of the channel, 0.25 dB steps, dBFS x 4
                          * (0 dBFS = a full-scale complex tone); SDR_LEVEL_NONE */
    uint8_t channel;     /* 0..7 */
    uint8_t flags;       /* SDR_RD_* */
} sdr_reading_t;

typedef struct {
    uint32_t tune_us, cap_us, wait_us, dsp_us;   /* this hop: next-LO tune, capture incl. the settle
                                                  * wait, the wait alone, PSD + levels */
} sdr_hop_timing_t;

typedef struct {
    uint8_t n;                         /* readings in r[]: 0 after a capture or DSP fault, or a stale capture */
    uint8_t hop;                       /* plan hop just measured (or retried, when stale) */
    bool rotation_end;                 /* this hop closed a rotation (every channel read once) */
    bool recalibrated;                 /* a recalibration ran in this step (any reason) */
    bool stale;                        /* the capture followed a PHY-tracking recalibration: retried */
    sdr_hop_timing_t t;
    sdr_reading_t r[SDR_RX_PER_HOP];
} sdr_hop_t;

typedef struct {
    uint32_t ch_khz[SDR_RX_CHANNELS];  /* default: Raceband 5658..5917 MHz */
    sdr_plan_mode_t mode;              /* default PAIRS */
    uint8_t gain;                      /* forced RX gain index; default CONFIG_SDR_RX_GAIN_DEFAULT */
} sdr_rx_config_t;

typedef struct {
    /* since boot */
    uint32_t hops, rotations;
    uint32_t cap_timeouts, cap_short, cap_overrun, dsp_errors;   /* faults = their sum (+1 if init failed) */
    uint32_t stale_caps;               /* captures dropped after a PHY-tracking recalibration (not a fault) */
    uint32_t recals;                   /* LO calibrations done (boot, commands, after a foreign one) */
    uint32_t foreign_dc;               /* DC measurements by someone else (PHY tracking) since init:
                                        * +3 per tracker recalibration (3 phy_set_rx_gain_cal_dc calls) */
    /* last 1 s window */
    uint16_t rot_hz_x10;               /* rotations per second x10 = refresh per channel */
    uint32_t rot_us_avg, rot_us_max;
    uint32_t tune_us_avg, tune_us_max, cap_us_avg, wait_us_avg, dsp_us_avg, dsp_us_max;
    uint16_t dc_max;                   /* worst |DC| (counts, max of I and Q means) over the plan's LOs */
    uint8_t iq_bad_mask;               /* LOs whose IQ estimate was implausible for most captures */
    /* state */
    uint8_t gain, gain_max;
    uint8_t nhops;
    uint16_t lo_mhz[SDR_RX_MAX_HOPS];
    bool calibrating, cal_needed, init_failed;
} sdr_rx_health_t;

/* app_main, once, after nvs_flash_init(): Wi-Fi as the PHY's host, exactly widscan58
 * main.c:76-83 / esp-sdr receiver.c:287-298: esp_event_loop_create_default();
 * WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(); esp_wifi_set_storage(WIFI_STORAGE_RAM);
 * esp_wifi_set_mode(WIFI_MODE_NULL); esp_wifi_start(); esp_wifi_set_ps(WIFI_PS_NONE);
 * esp_wifi_set_promiscuous(true); esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE); then, unless
 * CONFIG_SDR_RX_PROMISC_FILTER_ALL, esp_wifi_set_promiscuous_filter({.filter_mask = 0}) (C5VRX
 * rf.c:349-353: no frame reaches the MAC's buffers or the Wi-Fi task). Each step's error is
 * returned. Runs the PHY's boot RF calibration. */
esp_err_t sdr_rx_radio_start(void);
sdr_rx_config_t sdr_rx_config_default(void);
/* [scan] Releases the bank (0x60095004 bits [11:8] and 16 cleared, dump CTRL bit 31 cleared)
 * before its first access to 0x40820000, then bank layout, plan, the calibration of every
 * plan LO (~41 ms each, verified as SDR_RECAL_FULL), and LO 0 tuned. Prints one
 * "SDR cal ..." line per LO. On failure, health.init_failed and cal_needed are set and
 * sdr_rx_fault_count() counts it. */
esp_err_t sdr_rx_init(const sdr_rx_config_t *cfg);
/* [scan] One hop (2.1). Applies pending requests first (recalibration, gain). */
void sdr_rx_step(sdr_hop_t *out);
/* [any] Applied by the scan task at its next sdr_rx_step. false: a recalibration is
 * already pending or running. */
bool sdr_rx_request_recal(sdr_recal_t kind);
void sdr_rx_request_gain(uint8_t gain);
/* [any] */
bool sdr_rx_calibrating(void);
uint8_t sdr_rx_gain(void);                 /* the requested (saved) gain */
uint8_t sdr_rx_gain_max(void);
void sdr_rx_get_health(sdr_rx_health_t *out);
uint32_t sdr_rx_fault_count(void);
const char *sdr_rx_phy_version(void);
void sdr_rx_print(void);                                   /* the T line "SDR ..." */
/* [any] Quiesce the engine, then restart. Sequence (spec 2.5 "Reboot"): print the line, wait
 * 50 ms (USB drains), set the shutdown flag, take and release phy_get_lock() (waits out a
 * tune or capture in progress; none starts after it), clear dump CTRL bit 31 and
 * 0x60095004 &= ~0x10f00, esp_wifi_stop() (lock NOT held: esp_phy_disable takes it),
 * [download only] REG_SET_FIELD(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT, 1),
 * esp_restart(). reboot_to_download is the console key '`' and prints
 * "SDR download_boot=requested" (tools/enter_download.py waits for it); reboot is the
 * link's LP_CMD_REBOOT and prints "SDR reboot=requested". */
void sdr_rx_reboot_to_download(void) __attribute__((noreturn));
void sdr_rx_reboot(void) __attribute__((noreturn));
