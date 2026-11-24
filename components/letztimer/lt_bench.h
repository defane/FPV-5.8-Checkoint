/* The console's engine benches (K then s, z, x, n, p or S): timing, settle/residue, image
 * rejection, noise, the SINGLE plan and the scan recording. Each runs inside the scan task
 * (lt_scan.c), with no detection meanwhile; afterwards the scan task restores the production
 * settings (PAIRS plan, PLL tune, IQ states reset) and relearns the rests. Their buffers are the
 * RF dump bank's bench scratch (sdr_bench_scratch), their statistics hop_stats.h. Output lines:
 * SDRBENCH (s), SDRRESIDUE (z), SDRIMAGE (x), SDRNOISE (n), SDRPLAN (p), SCANBENCH and the
 * SCANDUMP read by tools/scan_analyze.py (S); each ends with a "... done" line. */
#pragma once
#include <stdbool.h>
#include "lt_internal.h"

#define LT_BENCH_KEYS "szxnpS"

/* A bench key. */
static inline bool lt_bench_key(char key)
{
    for (const char *k = LT_BENCH_KEYS; *k; k++)
        if (*k == key) return true;
    return false;
}

/* The scan task, between two sdr_rx_step calls: run the bench of `key` (lt_bench_key) to its
 * end. Every loop in it calls lt_yield(y). */
void lt_bench_run(lt_t *cp, char key, lt_yield_t *y);
