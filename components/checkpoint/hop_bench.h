/* Hop bench (console K then s) and scan recording (K then S): can one C5 scan
 * Raceband? See hop_bench.c. */
#pragma once
#include <stdbool.h>

/* Starts the bench in its own task; prints why when it refuses. */
void hop_bench_start(void);
/* Starts the scan recording, or stops it and prints what it recorded. */
void scan_bench_toggle(void);
/* Times what a strong channel leaves in the next window after a hop (K then z). */
void residue_bench_start(void);
/* The scan's gain, one step up or down (K then G or g). */
void scan_bench_gain(bool up);
/* checkpoint.c: true makes the detector's scan let go of the receiver and stay off,
 * false lets it start again. */
void checkpoint_scan_hold(bool hold);
