/* C5VRX-5 by Twotoz and the C5VRX contributors: headless menu for the
 * FPV 5.8 Checkpoint fork, video output off. Replaces upstream menu.c,
 * menu_ui.c and menu_raster.c (C5VRX commit 6bc9e4b): no raster (its
 * 100 KB DMA buffer), no descriptor chain, nothing drawn. Kept, because the
 * receive chain relies on them without a picture:
 *  - the splash flag: BOOT_PHASE runs only while menu_splash_active()
 *    (calibration.c boot_phase_check), and main.c waits under it for the
 *    first gain hold before checkpoint_start();
 *  - TX ownership: a splash or status screen stops live TX as the raster
 *    did, so calibrations and profile switches see the same "menu owns TX"
 *    state (s_menu_active); closing is the same verified live restart.
 * menu_open() refuses: there is no menu to navigate. The 150 ms relock gaps
 * only let a monitor re-sync; there is no monitor. */
#include "receiver.h"

volatile int s_menu_cursor;
volatile int s_exp_sub = -1;
bool s_exp_failed;
volatile int s_cal_sub = -1;

static volatile bool s_splash, s_status;

/* Live -> owned: TX stops, RX keeps collecting IQ for filter, DC and
 * profile calibration (transport_stop_tx). */
static void take_tx(void)
{
    if (s_menu_active) return;
    s_menu_active = true;
    transport_stop_tx();
}

/* Every caller checks !s_menu_active first and gives up on false. */
bool menu_open(void) { return false; }

void menu_close(void)
{
    if (!s_menu_active) return;
    transport_resume_live();
    s_menu_active = false;
    s_cal_sub = -1;
    s_exp_sub = -1;
    s_exp_failed = false;
    ++s_control_epoch;
}

void menu_render(void) {}
void menu_dac_step(void) {}
void menu_set_standard_mode(std_mode_t mode) { s_std_mode = mode; }

/* Never fails (upstream skipped the splash, and with it BOOT_PHASE, when the
 * raster chain did not fit in memory). */
bool menu_splash_begin(void)
{
    s_splash = true;
    take_tx();
    return true;
}

void menu_splash_progress(int percent) { (void)percent; }

void menu_splash_end(void)
{
    if (!s_splash) return;
    s_splash = false;
    menu_close();
}

bool menu_splash_active(void) { return s_splash; }

/* The text is not shown; the TX ownership is what calibrations rely on. */
void status_screen(const char *title, const char *line1, const char *line2, int percent)
{
    (void)title; (void)line1; (void)line2; (void)percent;
    if (s_status) return;
    s_status = true;
    take_tx();
}

void status_screen_end(void)
{
    if (!s_status) return;
    s_status = false;
    menu_close();
}

bool menu_status_active(void) { return s_status; }
