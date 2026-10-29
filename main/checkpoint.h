/* Checkpoint: "a drone has just passed" on the tuned channel, from the
 * receiver's noise-referenced C/N (receiver_obs_cn_x10, dB), with a coin
 * sound per pass when the audio build is enabled. Read-only on the receiver:
 * no GPIO, DMA, retune or gain write. */
#pragma once
#include <stdbool.h>

/* Call after the boot splash: the task and its state are allocated then,
 * not during the boot memory peak. */
void checkpoint_start(void);
void checkpoint_print(void);   /* T: CHECKPOINT line */

/* Console (console.c). */
void checkpoint_play_test(void);     /* P */
void checkpoint_toggle_mute(void);   /* p */
void checkpoint_help(void);          /* K */
bool checkpoint_key(char key);       /* the key after K; false: not a checkpoint key */
