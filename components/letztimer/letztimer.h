/* LetzTimer: "a drone has just passed" on any Raceband channel, from the sdr_rx engine's
 * readings of R1-R8 (one LO for two channels, ~500 readings a second per channel), reported over
 * the UART link (the sound is played by the C6 master). One scan task owns the engine and runs
 * the detector; it lets go of nothing: a calibration or a console bench runs inside it. The
 * detector's only GPIO are the link UART and, on request, the C5-Zero antenna switch. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

void letztimer_reserve(void);            /* app_main, first: scan task stack + TCB */
void letztimer_console_start(void);      /* USB reader + console tasks */
void letztimer_boot_stage(const char *stage);   /* HB "stage=" */
void letztimer_antenna_boot(void);       /* before sdr_rx_radio_start */
void letztimer_start(int64_t boot_t0_us);/* prefs, detector, scan task, link */
void letztimer_print(void);              /* T: LETZTIMER line */
void letztimer_help(void);               /* K */
bool letztimer_key(char key);            /* the key after K; false: not ours */
const char *firmware_id(void);
void firmware_print(void);
