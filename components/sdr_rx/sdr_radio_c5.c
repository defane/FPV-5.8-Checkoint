/* ESP32-C5 snapshot receiver backend.
 * Capture (the PHY I/Q dump engine), RX path setup, gain and analog filter control derived
 * from esp-sdr main/families/c5_c6_c61/receiver.c, main/targets/esp32c5/ and
 * main/common/burst_gain.h (https://github.com/ESPARGOS/esp-sdr),
 * GPL-3.0-or-later. The PLL-only retune (radio_tune) is our own.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to FPV 5.8 Checkpoint from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/radio/c5/radio_c5.c. widscan58 derived it from esp-sdr by ESPARGOS
 * (https://github.com/ESPARGOS/esp-sdr), commit a8eaac1, main/families/c5_c6_c61/receiver.c,
 * main/targets/esp32c5/ and main/common/burst_gain.h.
 * Reboot-to-download sequence after C5VRX-5 rf.c rf_reboot_to_download() (Twotoz and the C5VRX
 * contributors, https://github.com/Twotoz/C5VRX, 6bc9e4b, GPL-3.0-only).
 * Changes for FPV 5.8 Checkpoint: gain forced with libphy phy_force_rx_gain under the PHY lock
 * (requested code kept, clamped per apply; no librftest), CACHE-only recalibration (5 GHz) with a
 * per-LO recalibration, the bank layout of spec 2.3, radio_capture under the PHY lock with a
 * settle-wait out-param, split errors and a stale check after PHY tracking's recalibration, a
 * temperature read before a full channel setup (tsens_settle), a shutdown flag and the reboot;
 * no spurs, caps, ONCE/EVERY, idle/sleep, regwatch or tune-skip API.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only.
 *
 * Private libphy ABI used (re-verify at the next IDF bump; CMakeLists.txt pins the archive):
 * - functions: phy_set_chanfreq, phy_rfpll_set_adc_rate, phy_adc_rate_set, phy_rx_filter_mode,
 *   phy_set_channel_rfpll_freq_new, phy_set_pbus_mem, phy_set_rx_gain_table, phy_xtal_duty_set,
 *   phy_agc_max_gain_set, phy_stop_tx_tone, phy_pbus_workmode, phy_pbus_xpd_rx_on,
 *   phy_pbus_xpd_tx_off, phy_set_rxclk_en, phy_force_rx_gain, phy_chip_i2c_readReg,
 *   phy_i2c_writeReg, phy_write_gain_mem, phy_wifi_set_tx_gain_new, phy_chip_set_chan_ana,
 *   phy_set_rx_gain_cal_dc, phy_set_rx_gain_cal_iq, phy_adc_rate_cal_rxdc, and phy_param;
 * - phy_param offsets: +30 (int16 kHz correction), +42 (band: MHz >= 3001), +49 (crystal code),
 *   +148 (cal flags), +222 (IQ), +288 (u16 MHz), +290 (mode != 0), +291 (channel mode, u8),
 *   +292..294 (AGC max), +384 and +912 (DC tables), +1067/+1068 (tracker threshold override),
 *   +0/+1024 (temperature now / at the last calibration);
 * - +288 is load-bearing: pll_retune writes it, and PHY tracking's recalibration ends with
 *   phy_chip_set_chan(+288, +291), so the tracker retunes our LO rather than a stale one;
 * - the wrapped call sites (CMakeLists.txt): phy_set_rfpll_freq -> ets_delay_us(300) (and
 *   every esp_rom_delay_us, its alias: tsens_settle keeps the sensor's 300 us out of a tune),
 *   phy_wr_rx_gain_mem -> phy_write_gain_mem, phy_set_chan_misc_new -> phy_wifi_set_tx_gain_new,
 *   phy_bb_init and phy_cal_param_track -> phy_set_rx_gain_cal_dc (3 calls each),
 *   phy_set_rx_gain_cal_dc/_iq -> phy_chip_set_chan_ana. target_power_backoff ->
 *   phy_wifi_set_tx_gain_new is intra-object (unwrapped), a TX-power API only. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include "esp_attr.h"
#include "esp_phy_init.h"
#include "esp_private/phy.h"
#include "esp_private/sar_periph_ctrl.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "sdkconfig.h"
#include "soc/lp_aon_reg.h"
#include "soc/soc.h"
#include "sdr_radio.h"
#include "sdr_recal_c5.h"
#include "sdr_rx.h"

/* The dump engine writes 0x40830000, but the SRAM ownership bit hands the modem
 * the whole 128 KiB bank. Keep it out of the heap; sram_guard.ld keeps it out
 * of static sections. */
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, sdr_rf_dump);
#define IQ_BUFFER ((uint32_t *)0x40830000)
/* The lower half (spec 2.3): the psd workspace, the gain slots, the calibration cache and the
 * bench scratch, here rather than in static RAM or the heap. A capture leaves it alone; it is
 * not accessible during one. */
#define SDR_PSD_WS ((uint8_t *)SDR_BANK_PSD_WS)
#define SDR_GAIN_SLOTS ((uint8_t *)SDR_BANK_GAIN)
#define SDR_CAL_CACHE ((uint8_t *)SDR_BANK_CAL)
#define SDR_BENCH ((uint8_t *)SDR_BANK_BENCH)
#define IQ_WORDS 16380u
#define IQ_GUARD_WORDS 4u /* overrun sentinels after the last sample; ends at the bank end */
#define SRAM_OWNER_REG 0x60095004u
#define SRAM_OWNER_DUMP 0x00010f00u /* [11:8] owner, 16 (dump_capture sets them) */
#define GAIN_REG 0x600a702cu
#define FILL_WORD 0xa5a0055au
#define GUARD_WORD 0x5a5aa5a5u
/* PHY channel mode 1 runs the wide analog path (capacitor codes span
 * ~48..22 MHz); mode 0 covers 23..11 MHz. Raceband pair mode needs wide. */
#define CHANNEL_MODE 1u

_Static_assert(SDR_BANK_PSD_WS == SDR_BANK_START && SDR_BANK_PSD_WS + SDR_BANK_PSD_BYTES == SDR_BANK_GAIN &&
                   SDR_BANK_GAIN + SDR_BANK_GAIN_BYTES == SDR_BANK_CAL &&
                   SDR_BANK_CAL + SDR_BANK_CAL_BYTES == SDR_BANK_BENCH &&
                   SDR_BANK_BENCH + SDR_BANK_BENCH_BYTES == SDR_BANK_IQ,
               "bank regions");
_Static_assert(C5_CAL_MEM_BYTES <= SDR_BANK_CAL_BYTES, "cal cache region");
_Static_assert((IQ_WORDS + IQ_GUARD_WORDS) * 4u <= SDR_BANK_END - SDR_BANK_IQ, "capture buffer");
_Static_assert(SDR_BANK_START == 0x40820000u && SDR_BANK_IQ == 0x40830000u && SDR_BANK_END == 0x40840000u,
               "bank addresses (the reservation, IQ_BUFFER)");

extern void phy_set_chanfreq(unsigned mhz, unsigned mode);
extern unsigned char phy_param[];
extern void phy_rfpll_set_adc_rate(void);
extern void phy_adc_rate_set(unsigned);
extern void phy_rx_filter_mode(unsigned);
extern unsigned phy_set_channel_rfpll_freq_new(unsigned mhz, unsigned xtal, int offset_khz);
extern void phy_set_pbus_mem(unsigned, unsigned, unsigned);
extern void phy_set_rx_gain_table(unsigned, unsigned);
extern void phy_xtal_duty_set(unsigned);
extern void phy_agc_max_gain_set(unsigned, unsigned);
extern void phy_stop_tx_tone(unsigned);
extern void phy_pbus_workmode(void);
extern void phy_pbus_xpd_rx_on(unsigned);
extern void phy_pbus_xpd_tx_off(void);
extern void phy_set_rxclk_en(unsigned);
/* libphy phy_reg.o: the index into 0x600a702c[31:24], then the force bit 23 = enable. It writes
 * the same register as librftest's force_rx_gain(enable, index, 0) (spec 2.6). */
extern void phy_force_rx_gain(unsigned enable, unsigned index);
extern unsigned phy_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void phy_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);

/* Rate code r is adctrig's divider argument 2r (esp-sdr LIMITS?). */
static const uint32_t rates_hz[RADIO_MAX_RATES] = {80000000, 40000000, 20000000, 10000000, 8000000, 4000000};

/* Bit 0: PLL-only retune between full ones (pll_retune); bit 1 also skips
 * the RX path re-apply (the PLL-only tune leaves it alone; same levels).
 * Bit 2 skips the gain re-apply, but both kinds of tune reset the manual
 * gain (+27 dB with TUNESKIP 7). A full tune runs only after a fresh
 * calibration (ours, or after PHY tracking's) and on the first tune: 10
 * minutes without one left every gain table bit for bit as a fresh build
 * and every channel within 0.12 dB (PLAN.md, periodic full tunes). */
static const unsigned tune_skip = 3;
static bool bench_full_tune; /* TUNESKIP 0, the K s bench's "full" method */
static bool tuned_once;
static uint32_t tuned_lo_khz;
static int64_t tune_end_us; /* when the LO last moved; radio_capture settles from it */
static unsigned s_gain_req = CONFIG_SDR_RX_GAIN_DEFAULT; /* the requested index: every apply clamps a copy */
static int filter_code = -1;
static unsigned calibrated_mhz;
static uint32_t cal_gen = 1; /* bumped whenever the DC/IQ tables may have changed */
static unsigned s_foreign_at_tune; /* c5_foreign_dc_count() when the last tune ended */
static volatile bool s_shutdown;    /* the reboot path took the lock: no tune or capture after it */
static volatile bool s_calibrating; /* c5_recalibrate is running */
static volatile uint32_t s_recals;  /* LO calibrations since boot */

static IRAM_ATTR unsigned gain_max(void) {
    unsigned maximum = (REG_READ(GAIN_REG) >> 8) & 127u;
    return maximum < 90u ? maximum : 0u; /* never expose uncalibrated slots */
}

/* After a tune (both kinds clear the force bit): one write. */
static IRAM_ATTR void gain_apply(void) {
    unsigned code = s_gain_req, maximum = gain_max();
    if (code > maximum) code = maximum;
    phy_force_rx_gain(1, code);
}

const char *radio_phy_version(void) { return get_phy_version_str(); }

void radio_force_recal(void) {
    calibrated_mhz = 0;
    cal_gen++;
    c5_cal_clear();
}

/* Channel setup is ~1 ms; two parts of it do nothing for a receiver and are
 * skipped while radio_tune runs it (linker --wrap, see CMakeLists.txt):
 * - phy_set_rfpll_freq ends with a fixed 300 us wait. The PLL calibration is
 *   restarted and checked again by phy_wait_rfpll_cal_end_new right after
 *   (restart, 20 us, VCO code check, retried while out of range), so the
 *   wait only delays that. Without it, spurs land within 4 kHz of their
 *   frequency right after 50-350 MHz jumps (300 captures, none off; the
 *   stock sequence had 2 off-frequency captures, raised-floor bursts).
 * - phy_wifi_set_tx_gain_new (~85 us): the TX gain table.
 * Spectra (floor, lines, DC) match the full sequence at 5677-5899 MHz;
 * PLAN.md, tune time on the C5.
 * The wrapper gates on the value alone, and esp_rom_delay_us is a linker alias of
 * ets_delay_us (esp32c5.rom.api.ld), so it resolves to the wrapper too: every 300 us
 * delay while rx_tuning is skipped. Besides the PLL wait, a full channel setup has one
 * more: phy_chip_set_chan reads the temperature sensor (phy_tsens_temp_read, into
 * phy_param +0, which PHY tracking compares with +1024), and a read that switches the
 * sensor's range waits 300 us before reading again (temperature_sensor_hal.c). tsens_settle()
 * does that read just before, with its wait. */
static volatile bool rx_tuning;

/* Outside rx_tuning, under the PHY lock: any range switch happens here with its settle, so
 * the read inside the channel setup ~0.1 ms later is in range. A register read otherwise. */
static void tsens_settle(void) { (void)temp_sensor_get_raw_value(NULL); }

extern void __real_ets_delay_us(uint32_t us);
IRAM_ATTR void __wrap_ets_delay_us(uint32_t us) {
    if (rx_tuning && us == 300) return;
    __real_ets_delay_us(us);
}
/* Prototype unknown; passes the argument registers through (RV32 ABI). */
extern uint32_t __real_phy_wifi_set_tx_gain_new(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                uint32_t);
uint32_t __wrap_phy_wifi_set_tx_gain_new(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5,
                                         uint32_t a6, uint32_t a7) {
    if (rx_tuning) return 0;
    return __real_phy_wifi_set_tx_gain_new(a0, a1, a2, a3, a4, a5, a6, a7);
}

/* The RX gain memory (84 entries) is built per LO from the DC tables by
 * phy_set_rx_gain_table: ~200 us, nearly all of it computing the entries,
 * ~35 us writing them. It also sets the AGC's maximum gain, which differs
 * between LOs and moves what a manual gain index means (2-5 dB at R1/R2 with
 * RECALMODE ONCE when it was left out). A PLL-only retune replays both as
 * recorded by the last build (channel setup's in a full tune, or its own).
 * The build reads only the band, the LO and the calibration, so a slot is
 * valid until the calibration changes (cal_gen); a replay matched a fresh
 * build bit for bit after 10 minutes at all 4 LOs of R1-R8.
 * Recording only takes 84 writes to entries 0..83 in order: the TX gain
 * table also goes through phy_write_gain_mem, but channel setup's
 * (phy_wifi_set_tx_gain_new) is skipped while rx_tuning. */
#define GAIN_MEM_N 84u
#define GAIN_SLOTS 8u /* one per LO of an 8-channel plan */
typedef struct {
    uint16_t mhz; /* 0 = empty */
    uint32_t gen, used;
    uint32_t w0[GAIN_MEM_N], w1[GAIN_MEM_N];
    uint8_t w2[GAIN_MEM_N];
    uint8_t agc_max[3]; /* phy_param +292..294 */
} gain_slot_t;
_Static_assert(GAIN_SLOTS * sizeof(gain_slot_t) <= SDR_BANK_GAIN_BYTES, "gain slots");
static gain_slot_t *gain_slots; /* GAIN_SLOTS, in the bank (radio_init) */
static gain_slot_t *gain_rec;   /* recording into this slot */
static unsigned gain_rec_n;
static bool gain_rec_ok;
static uint32_t gain_clock;
static unsigned gain_mhz; /* what the gain memory holds: built for this LO ... */
static uint32_t gain_gen; /* ... from this calibration */

/* Prototype unknown (four register values, the last one the entry index). */
extern void __real_phy_write_gain_mem(uint32_t, uint32_t, uint32_t, uint32_t);
IRAM_ATTR void __wrap_phy_write_gain_mem(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    __real_phy_write_gain_mem(a0, a1, a2, a3);
    if (!gain_rec) return;
    if (a3 != gain_rec_n || gain_rec_n >= GAIN_MEM_N || a2 > 255u) {
        gain_rec_ok = false;
        return;
    }
    gain_rec->w0[gain_rec_n] = a0;
    gain_rec->w1[gain_rec_n] = a1;
    gain_rec->w2[gain_rec_n++] = (uint8_t)a2;
}

/* The slot holding this LO's table for the current calibration, or NULL. */
static IRAM_ATTR gain_slot_t *gain_slot_for(unsigned mhz) {
    for (unsigned j = 0; gain_slots && j < GAIN_SLOTS; j++)
        if (gain_slots[j].mhz == mhz && gain_slots[j].gen == cal_gen) return &gain_slots[j];
    return NULL;
}

/* An empty or stale slot, else the least recently used one. */
static gain_slot_t *gain_slot_victim(void) {
    gain_slot_t *victim = NULL;
    for (unsigned j = 0; gain_slots && j < GAIN_SLOTS; j++) {
        gain_slot_t *slot = &gain_slots[j];
        if (!victim || (victim->gen == cal_gen && (slot->gen != cal_gen || slot->used < victim->used))) victim = slot;
    }
    return victim;
}

static void gain_rec_start(gain_slot_t *slot) {
    gain_rec = slot;
    gain_rec_n = 0;
    gain_rec_ok = true;
}

/* After the table build: keep the recording if it is complete. */
static void gain_rec_end(unsigned mhz) {
    gain_slot_t *slot = gain_rec;
    gain_rec = NULL;
    if (!slot) return;
    slot->mhz = gain_rec_ok && gain_rec_n == GAIN_MEM_N ? (uint16_t)mhz : 0;
    memcpy(slot->agc_max, phy_param + 292, sizeof(slot->agc_max));
    slot->gen = cal_gen;
    slot->used = ++gain_clock;
}

static IRAM_ATTR void gain_table_load(unsigned mhz) {
    gain_slot_t *slot = gain_slot_for(mhz);
    if (slot) {
        for (unsigned k = 0; k < GAIN_MEM_N; k++) __real_phy_write_gain_mem(slot->w0[k], slot->w1[k], slot->w2[k], k);
        /* The end of phy_set_rx_gain_table. */
        memcpy(phy_param + 292, slot->agc_max, sizeof(slot->agc_max));
        phy_agc_max_gain_set(phy_param[42] ? phy_param[294] : phy_param[293], phy_param[292]);
        slot->used = ++gain_clock;
    } else {
        gain_rec_start(slot ? slot : gain_slot_victim());
        phy_set_rx_gain_table(mhz, 0);
        gain_rec_end(mhz);
    }
    gain_mhz = mhz;
    gain_gen = cal_gen;
}

/* PLL-only retune: what phy_chip_set_chan does that depends on the frequency
 * in 5 GHz RX, without the rest (AGC, TX power, DC memory clear, PHY
 * registers that only change with band or bandwidth):
 * - phy_rfpll_set_adc_rate: ADC rate and RX filter mode (adc_path_set then
 *   puts every LO on the above-5830 MHz setting);
 * - phy_set_channel_rfpll_freq_new, the PLL itself (also phy_chip_set_chan_ana):
 *   band, SDM, 5 GHz ckgen and PLL calibration, frequency registers. Passed
 *   the kHz correction (+30) and crystal code (+49) as channel setup does;
 * - the pbus memory (channel caps, mixer gain) and the crystal duty;
 * - the RX gain table, replayed from gain_slots where possible.
 * With the gain re-apply, a hop's tune takes ~0.3 ms instead of ~0.5-0.6 ms
 * (PLAN.md, PLL-only retune on the C5). */
static IRAM_ATTR void pll_retune(unsigned mhz, bool restored) {
    *(uint16_t *)(phy_param + 288) = (uint16_t)mhz;
    phy_rfpll_set_adc_rate();
    phy_set_channel_rfpll_freq_new(mhz, phy_param[49], *(int16_t *)(phy_param + 30));
    phy_set_pbus_mem(phy_param[290], mhz, phy_param[42]);
    if (restored || gain_mhz != mhz || gain_gen != cal_gen) gain_table_load(mhz);
    phy_xtal_duty_set(mhz);
}

/* Raceband only: the 2.4 GHz paths of widscan58 (full channel setup per tune, one ISM
 * calibration) are not ported, so radio_tune refuses LOs below this. */
#define BAND_5G_MHZ 4900u

/* The PHY runs 5 GHz at ADC rate 0 with no decimation filter (RX filter mode
 * 0), and only above 5830 MHz at rate 1 with filter mode 8
 * (phy_rfpll_set_adc_rate). Rate 1 + mode 8 everywhere: a VTX 50-60 MHz from
 * the LO aliases 24-30 dB down at rate 0 and below the noise here, the odd
 * 80 MHz board spurs (5680, 5840 MHz) go away, SNR is unchanged; the price is
 * the skirt R7/R8 always had (-3 dB at +-24 MHz, ~0.8 dB at a pair channel).
 * Set after every tune, before the settle and gain apply: switching the rate
 * only around a capture lost 5-8 dB (PLAN.md, the 5830/5831 MHz skirt). */
#define ADC_RATE1_ABOVE_MHZ 5830u
static IRAM_ATTR void adc_path_set(unsigned mhz) {
    if (mhz < BAND_5G_MHZ || mhz > ADC_RATE1_ABOVE_MHZ) return; /* the PHY's own setting */
    phy_adc_rate_set(1);
    phy_rx_filter_mode(8);
}

static IRAM_ATTR void rx_path_on(void) {
    phy_stop_tx_tone(1);
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    phy_pbus_xpd_rx_on(1);
    phy_set_rxclk_en(1);
}

/* The end of every tune: the capture's stale check compares PHY tracking's DC count with this. */
static IRAM_ATTR void tune_done(uint32_t lo_khz) {
    tuned_once = true;
    tuned_lo_khz = lo_khz;
    tune_end_us = esp_timer_get_time();
    s_foreign_at_tune = c5_foreign_dc_count();
}

static IRAM_ATTR esp_err_t tune_locked(uint32_t lo_khz) {
    /* Tracking recalibrated at its reference channels: our tables, cached
     * ones included, are stale (they also predate a temperature change). */
    if (c5_cal_replaced()) {
        calibrated_mhz = 0;
        cal_gen++;
        c5_cal_clear();
    }
    const unsigned skip = bench_full_tune ? 0u : tune_skip;
    unsigned mhz = lo_khz / 1000u;
    bool recalibrated = false, restored = false;
    if (calibrated_mhz != mhz) {
        restored = c5_cal_restore(mhz);
        if (!restored) {
            s_calibrating = true;
            c5_recalibrate(mhz);
            s_calibrating = false;
            s_recals++;
            cal_gen++;
            c5_cal_save(mhz);
            recalibrated = true;
        }
        calibrated_mhz = mhz;
    }
    bool pll_only = skip & 1u;
    bool full = !tuned_once || recalibrated;
    if (!full && !restored && lo_khz == tuned_lo_khz) {
        s_foreign_at_tune = c5_foreign_dc_count();
        return ESP_OK;
    }
    if (full || !pll_only) tsens_settle();
    rx_tuning = true;
    if (full || !pll_only) {
        /* Channel setup rebuilds the gain table: record it for the PLL-only tunes. */
        if (pll_only) {
            gain_slot_t *slot = gain_slot_for(mhz);
            gain_rec_start(slot ? slot : gain_slot_victim());
        }
        phy_set_chanfreq(mhz, CHANNEL_MODE);
        if (pll_only) gain_rec_end(mhz);
        gain_mhz = mhz;
        gain_gen = cal_gen;
    } else {
        pll_retune(mhz, restored);
    }
    adc_path_set(mhz);
    rx_tuning = false;
    if (full || !(skip & 2u)) rx_path_on();
    if (full || !(skip & 4u)) gain_apply();
    tune_done(lo_khz);
    return ESP_OK;
}

/* Under the PHY lock: PHY tracking (an esp_timer callback every second) takes
 * it, and its calibration interleaved with ours hung the scanner within
 * seconds (PLAN.md, PHY tracking on the C5). */
IRAM_ATTR esp_err_t radio_tune(uint32_t lo_khz) {
    if (lo_khz % 1000u || lo_khz < BAND_5G_MHZ * 1000u || lo_khz > 6000000u) return ESP_ERR_INVALID_ARG;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    esp_err_t err = s_shutdown ? ESP_ERR_INVALID_STATE : tune_locked(lo_khz);
    _lock_release(&lock);
    return err;
}

/* The next tune of that LO misses the cache: a plain calibrated_mhz = 0 would only restore the
 * entry being replaced. */
esp_err_t radio_recal_lo(uint32_t lo_khz) {
    if (lo_khz % 1000u || lo_khz < BAND_5G_MHZ * 1000u || lo_khz > 6000000u) return ESP_ERR_INVALID_ARG;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_shutdown) {
        c5_cal_forget(lo_khz / 1000u);
        calibrated_mhz = 0;
        err = tune_locked(lo_khz);
    }
    _lock_release(&lock);
    return err;
}

void radio_set_filter(int code) { filter_code = code > 63 ? 63 : code; }

void radio_set_gain(unsigned code) { s_gain_req = code; }
unsigned radio_gain(void) { return s_gain_req; }
unsigned radio_gain_max(void) { return gain_max(); }

/* A gain applied without a tune: the force bit is still set, so pulse it low, then high, as
 * esp-sdr's rx_gain_force (main/common/rx_gain_force.h), and settle from now. Under the lock:
 * PHY tracking's channel setup writes the same register. */
IRAM_ATTR esp_err_t radio_gain_now(unsigned code) {
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_shutdown) {
        unsigned maximum = gain_max();
        if (code > maximum) code = maximum;
        phy_force_rx_gain(0, code);
        esp_rom_delay_us(2);
        phy_force_rx_gain(1, code);
        tune_end_us = esp_timer_get_time();
        err = ESP_OK;
    }
    _lock_release(&lock);
    return err;
}

/* BBTOP 0x67 host 1 regs 6/7 low six bits: RX analog filter capacitor code.
 * Applied only around a snapshot and restored before any retune, so PHY
 * calibration always sees its own setting. */
static unsigned filter_saved[2];
static IRAM_ATTR void filter_apply(void) {
    for (unsigned j = 0; j < 2; j++) {
        filter_saved[j] = phy_chip_i2c_readReg(0x67, 1, 6 + j);
        if (filter_code >= 0)
            phy_i2c_writeReg(0x67, 1, 6 + j, (filter_saved[j] & ~63u) | (unsigned)filter_code);
    }
}
static IRAM_ATTR void filter_restore(void) {
    if (filter_code >= 0)
        for (unsigned j = 0; j < 2; j++) phy_i2c_writeReg(0x67, 1, 6 + j, filter_saved[j]);
}

/* The dump engine, driven directly: adctrig(n - 1, 0, 0, 2 * rate, 0, ...)
 * (librftest mac_common.o, IDF 5.5.5) unrolled for selector 0 (raw source 15,
 * software trigger, no tone). adctrig also clears bit 0 of 0x600a20b4, a
 * register outside any clocked block here: each read or write of it stalls
 * the bus ~840 us (it reads 0), which was 1.65 ms of every 1.79 ms capture.
 * Without it the trigger-to-done wait is ~52 us for 4096 samples. */
#define DUMP_CTRL_REG 0x600a9004u /* [16:0] length, 17, 18 done, 19 trigger, [21:20], 31 enable */
#define DUMP_CONF_REG 0x600a9008u /* [20:17] source, [23:21] rate code */
#define DUMP_PACK_REG 0x600a9018u /* four 6-bit lane selectors, 24 enable */
static IRAM_ATTR bool dump_capture(unsigned n, unsigned rate_code) {
    REG_WRITE(DUMP_CONF_REG, (REG_READ(DUMP_CONF_REG) & ~(7u << 21)) | ((rate_code & 7u) << 21));
    REG_WRITE(0x600a9c04u, 0xffffffffu);
    REG_SET_BIT(0x600a0800u, 4u);
    REG_WRITE(DUMP_CTRL_REG, (REG_READ(DUMP_CTRL_REG) & ~0x3ffffu) | n);
    REG_WRITE(DUMP_PACK_REG, (REG_READ(DUMP_PACK_REG) & ~0xffffffu) | (27u << 18) | (26u << 12) | (25u << 6) | 24u);
    REG_SET_BIT(DUMP_PACK_REG, 1u << 24);
    REG_WRITE(SRAM_OWNER_REG, (REG_READ(SRAM_OWNER_REG) & ~(15u << 8)) | (2u << 8) | (1u << 16));
    REG_CLR_BIT(DUMP_CTRL_REG, 3u << 20);
    REG_SET_BIT(DUMP_CONF_REG, 15u << 17);
    REG_SET_BIT(DUMP_CTRL_REG, 1u << 31);
    REG_SET_BIT(DUMP_CTRL_REG, 1u << 19);
    REG_CLR_BIT(DUMP_CTRL_REG, 1u << 19);
    int64_t start = esp_timer_get_time();
    while (!(REG_READ(DUMP_CTRL_REG) & (1u << 18)) && esp_timer_get_time() - start < 20000) {}
    bool done = (REG_READ(DUMP_CTRL_REG) & (1u << 18)) != 0;
    REG_CLR_BIT(DUMP_CTRL_REG, 1u << 31);
    return done;
}

uint32_t radio_rate_hz(unsigned rate_code) { return rate_code < RADIO_MAX_RATES ? rates_hz[rate_code] : 0; }

/* The settle wait stays outside the PHY lock; the filter, the dump and the restore run under it
 * (~25 us), so PHY tracking's analog I2C writes and BBPLL calibration never interleave with
 * filter_apply's read-modify-write or run during the dump. A tracking recalibration since the
 * tune left the LO unforced and on the PHY's own ADC path: such a capture is refused ("stale"). */
IRAM_ATTR esp_err_t radio_capture(unsigned n, unsigned rate_code, uint32_t settle_us, const uint32_t **words,
                                  int64_t *t_mid_us, uint32_t *elapsed_us, uint32_t *waited_us) {
    if (n < 16 || n > IQ_WORDS || rate_code >= RADIO_MAX_RATES) return ESP_ERR_INVALID_ARG;
    for (unsigned j = 0; j < n; j++) IQ_BUFFER[j] = FILL_WORD;
    for (unsigned j = 0; j < IQ_GUARD_WORDS; j++) IQ_BUFFER[n + j] = GUARD_WORD ^ j;
    const int64_t wait_from = esp_timer_get_time();
    while (esp_timer_get_time() - tune_end_us < (int64_t)settle_us) {}
    if (waited_us) *waited_us = (uint32_t)(esp_timer_get_time() - wait_from);
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    if (s_shutdown) {
        _lock_release(&lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (c5_foreign_dc_count() != s_foreign_at_tune) {
        _lock_release(&lock);
        return ESP_ERR_INVALID_RESPONSE;
    }
    filter_apply();
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    int64_t start = esp_timer_get_time();
    /* esp-sdr: do NOT set CTRL bit 17 as on the C61; on the C5 that gave a
     * short, incomplete snapshot. */
    bool done = dump_capture(n, rate_code);
    uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    REG_WRITE(SRAM_OWNER_REG, owner);
    filter_restore();
    _lock_release(&lock);
    if (!done) return ESP_ERR_TIMEOUT;
    /* An unwritten fill word means the dump stopped early; a changed guard
     * word means it ran past n. */
    if (IQ_BUFFER[n - 1] == FILL_WORD || IQ_BUFFER[n / 2] == FILL_WORD || IQ_BUFFER[0] == FILL_WORD)
        return ESP_ERR_INVALID_SIZE;
    for (unsigned j = 0; j < IQ_GUARD_WORDS; j++)
        if (IQ_BUFFER[n + j] != (GUARD_WORD ^ j)) return ESP_ERR_INVALID_STATE;
    *words = IQ_BUFFER;
    if (t_mid_us) *t_mid_us = start + ((int64_t)n * 500000 + rates_hz[rate_code] / 2) / rates_hz[rate_code];
    if (elapsed_us) *elapsed_us = elapsed;
    return ESP_OK;
}

bool radio_calibrating(void) { return s_calibrating; }
uint32_t radio_recals(void) { return s_recals; }
unsigned radio_foreign_dc(void) { return c5_foreign_dc_count(); }

void radio_set_full_tune(bool on) {
    bench_full_tune = on;
    tuned_once = false;
}

/* ONCE emulation for the K n bench: the DC/IQ areas cached for another LO, then a full channel
 * setup, which builds this LO's gain table from them (not recorded). */
esp_err_t radio_borrow_cal(uint32_t lo_khz, uint32_t cal_from_khz) {
    if (lo_khz % 1000u || lo_khz < BAND_5G_MHZ * 1000u || lo_khz > 6000000u || cal_from_khz % 1000u)
        return ESP_ERR_INVALID_ARG;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_shutdown) {
        if (c5_cal_replaced()) {
            calibrated_mhz = 0;
            cal_gen++;
            c5_cal_clear();
        }
        err = ESP_ERR_NOT_FOUND;
        if (c5_cal_restore(cal_from_khz / 1000u)) {
            const unsigned mhz = lo_khz / 1000u;
            cal_gen++;          /* no gain slot keeps a table built from the borrowed areas */
            calibrated_mhz = 0; /* any later tune restores the LO's own entry */
            tsens_settle();
            rx_tuning = true;
            phy_set_chanfreq(mhz, CHANNEL_MODE);
            gain_mhz = mhz;
            gain_gen = cal_gen;
            adc_path_set(mhz);
            rx_tuning = false;
            rx_path_on();
            gain_apply();
            tune_done(lo_khz);
            err = ESP_OK;
        }
    }
    _lock_release(&lock);
    return err;
}

void radio_borrow_end(void) {
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    cal_gen++;
    calibrated_mhz = 0;
    tuned_lo_khz = 0;
    _lock_release(&lock);
}

/* The bank's first access must find it released: a reset can leave the dump enabled and the
 * modem owning the bank (0x60095004 is an HP_SYSTEM register, outside the modem reset). */
esp_err_t radio_init(void) {
    REG_CLR_BIT(DUMP_CTRL_REG, 1u << 31);
    REG_WRITE(SRAM_OWNER_REG, REG_READ(SRAM_OWNER_REG) & ~SRAM_OWNER_DUMP);
    gain_slots = (gain_slot_t *)SDR_GAIN_SLOTS;
    memset(gain_slots, 0, GAIN_SLOTS * sizeof(gain_slot_t));
    c5_cal_init(SDR_CAL_CACHE);
    calibrated_mhz = 0;
    cal_gen++;
    tuned_once = false;
    tuned_lo_khz = 0;
    gain_mhz = 0;
    return ESP_OK;
}

/* The dump only writes the upper half of the bank. The lower half is reserved
 * just because the modem owns the whole bank during a capture; its contents
 * survive captures (checked on hardware: 4000+ captures of every size and
 * rate, recalibration on every tune, hardware AGC). */
void *radio_dsp_region(size_t *bytes) {
    if (bytes) *bytes = SDR_BANK_PSD_BYTES;
    return SDR_PSD_WS;
}

void *radio_bench_region(size_t *bytes) {
    if (bytes) *bytes = SDR_BANK_BENCH_BYTES;
    return SDR_BENCH;
}

uint32_t *radio_sample_buffer(unsigned *max_words) {
    if (max_words) *max_words = 0x10000u / 4u; /* 0x40830000 up to the end of the reserved bank */
    return IQ_BUFFER;
}

/* Quiesce, then restart (spec 2.5). The caller (the console's USB reader or the link task)
 * blocks in the delay and in esp_wifi_stop, while the scan task would re-arm the dump and the
 * bank owner at every capture: the flag, checked under the lock, stops that for good once the
 * lock has been taken and released here. esp_wifi_stop takes the same (non-recursive) lock. */
static void __attribute__((noreturn)) reboot(bool download) {
    /* esptool's USB reset does not reset the modem domain; the forced clocks
     * and the running dump writer would keep the ROM loader from answering. */
    fputs(download ? "SDR download_boot=requested\n" : "SDR reboot=requested\n", stdout);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    s_shutdown = true;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    _lock_release(&lock);
    REG_CLR_BIT(DUMP_CTRL_REG, 1u << 31);
    REG_WRITE(SRAM_OWNER_REG, REG_READ(SRAM_OWNER_REG) & ~SRAM_OWNER_DUMP);
    (void)esp_wifi_stop();
    if (download) REG_SET_FIELD(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT, 1u);
    esp_restart();
}

void sdr_rx_reboot_to_download(void) { reboot(true); }
void sdr_rx_reboot(void) { reboot(false); }
