/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to FPV 5.8 Checkpoint from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/radio/c5/recal_c5.h. widscan58 derived it from esp-sdr by ESPARGOS
 * (https://github.com/ESPARGOS/esp-sdr), commit a8eaac1, main/common/rx_recalibration.c.
 * Changes for FPV 5.8 Checkpoint: c5_foreign_dc_count, c5_cal_forget and C5_CAL_MEM_BYTES added.
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
void c5_recalibrate(unsigned mhz);

/* True once after the PHY replaced the RX calibration on its own (tracking);
 * call under the PHY lock. */
bool c5_cal_replaced(void);
/* DC measurements made by anyone but c5_recalibrate since boot (PHY tracking: 3 per
 * recalibration, and 3 for the boot calibration). Does not consume them as c5_cal_replaced does. */
unsigned c5_foreign_dc_count(void);

/* Per-LO calibration cache (5 GHz only): restore a saved calibration (the
 * next phy_set_chanfreq rebuilds the gain table from it), or save the
 * current one. */
bool c5_cal_restore(unsigned mhz);
void c5_cal_save(unsigned mhz);
void c5_cal_clear(void);
/* Drop one LO's saved calibration, so that its next tune calibrates it again. */
void c5_cal_forget(unsigned mhz);
/* Its memory (c5_cal_bytes), cleared here; without it nothing is cached. */
size_t c5_cal_bytes(void);
void c5_cal_init(void *mem);
/* What c5_cal_init's memory must hold at least (c5_cal_bytes() fits it, static-asserted). */
#define C5_CAL_MEM_BYTES 6144u
