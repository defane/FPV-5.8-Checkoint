/* Checkpoint: "a drone has just passed" on the tuned channel, from the
 * receiver's noise-referenced C/N (receiver_obs_cn_x10, dB), reported over the
 * UART link (the sound is played by the C6 master). It only reads the receiver,
 * except what the bridge asks for over the link: a retune (SET_CHANNEL, saved)
 * and a calibration (filter_request / filter_request_full) or reboot (COMMAND).
 * No GPIO, DMA or gain write. */
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
