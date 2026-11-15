/* The RF dump owns the whole 128 KiB SRAM bank 0x40820000..0x40840000 while a
 * capture runs (C5 ownership bit 1 covers the entire bank). Heap reservation
 * alone does not stop static sections growing into it. Derived from esp-sdr
 * main/targets/esp32c5/sram_guard.ld.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Ported to FPV 5.8 Checkpoint from widscan58 by CapnBry (https://github.com/CapnBry/widscan58),
 * commit a9fff97, firmware/src/radio/c5/sram_guard.ld. widscan58 derived it from esp-sdr by ESPARGOS
 * (https://github.com/ESPARGOS/esp-sdr), commit a8eaac1, main/targets/esp32c5/sram_guard.ld.
 * Changes for FPV 5.8 Checkpoint: none (verbatim).
 * This file keeps its GPL-3.0-or-later notice; the repository as a whole is GPL-3.0-only. */
ASSERT(_iram_end <= 0x40820000, "RF dump bank overlaps IRAM")
ASSERT(_data_end <= 0x40820000, "RF dump bank overlaps data")
ASSERT(_bss_end <= 0x40820000, "RF dump bank overlaps BSS")
