/* Checkpoint: "a drone has just passed" on any Raceband channel, from a continuous
 * sweep of R1-R8, reported over the UART link (the sound is played by the C6 master).
 * It owns the receiver like scan(), and lets go of it for a calibration
 * (filter_request / filter_request_full, COMMAND) or a console bench. No GPIO or DMA. */
#pragma once
#include <stdbool.h>

/* Call after the boot splash: the task and its state are allocated then,
 * not during the boot memory peak. */
void checkpoint_start(void);
/* Call before the radio starts: the antenna the calibration will see. */
void checkpoint_antenna_boot(void);
void checkpoint_print(void);   /* T: CHECKPOINT line */

/* Console (console.c). */
void checkpoint_help(void);          /* K */
bool checkpoint_key(char key);       /* the key after K; false: not a checkpoint key */
