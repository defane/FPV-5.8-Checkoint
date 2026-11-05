/* Hop bench (console K then s) and scan prototype (K then S): can one C5 scan
 * Raceband? See hop_bench.c. */
#pragma once
#include <stdbool.h>

/* Starts the bench in its own task; prints why when it refuses. */
void hop_bench_start(void);
/* Starts the scan prototype, or stops it and prints what it recorded. */
void scan_bench_toggle(void);
/* The scan prototype's gain, one step up or down for the next scan (K then G or g). */
void scan_bench_gain(bool up);
