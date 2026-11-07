/* The Raceband sweep. See scan_hop.h.
 *
 * phy_set_freq() alone retunes in under a millisecond, where rf_set_channel() goes
 * through the Wi-Fi driver and the whole lock, but it drops the forced gain, so that
 * is forced again after it: the `force` hop of the bench (docs/bench.md). It calls
 * libphy past rf.c, operator-approved; the protected receive chain is not edited,
 * only its public calls and the libphy functions rf.c uses. */
#include "scan_hop.h"
#include "esp_timer.h"
#include "hop_stats.h"
#include "phy.h"
#include "rf.h"
#include <stdlib.h>

extern void phy_set_freq(uint16_t freq_mhz, int offset);   /* libphy, as in rf.c */

const uint8_t scan_order[SCAN_CHANNELS] = {0, 4, 1, 5, 2, 6, 3, 7};
static uint8_t s_scan_gain = SCAN_GAIN_DEFAULT;

uint8_t scan_gain(void) { return s_scan_gain; }

void scan_gain_step(bool up)
{
    int g = (int)s_scan_gain + (up ? (int)SCAN_GAIN_STEP : -(int)SCAN_GAIN_STEP);
    if (g < (int)SCAN_GAIN_MIN) g = (int)SCAN_GAIN_MIN;
    if (g > (int)SCAN_GAIN_MAX) g = (int)SCAN_GAIN_MAX;
    s_scan_gain = (uint8_t)g;
}

bool scan_receiver_free(void)
{
    return !(s_menu_active || s_scan_active || menu_status_active() || calibration_in_startup() ||
             receiver_paused() || !receiver_active());
}

bool scan_take(const char *tag, uint8_t gain, scan_saved_t *saved)
{
    if (!receiver_pause(tag)) return false;
    *saved = (scan_saved_t){rf_channel_index(), rf_offset_khz(), s_current_gain};
    s_scan_active = true;
    receiver_set_gain(gain);
    return true;
}

/* Back by the far end of the band: esp_wifi_set_channel() may do nothing for the
 * Wi-Fi channel it believes is current, and direct hops left it stale. */
unsigned scan_give(const scan_saved_t *saved)
{
    const int f = rf_channel_freq(saved->channel);
    const size_t detour = abs(f - rf_channel_freq(0)) > abs(f - rf_channel_freq(SCAN_CHANNELS - 1u)) ? 0u : SCAN_CHANNELS - 1u;
    const unsigned failed = rf_set_channel(detour) != ESP_OK || rf_set_channel(saved->channel) != ESP_OK;
    rf_set_offset_khz(saved->offset_khz);
    receiver_set_gain(saved->gain);
    s_scan_active = false;
    receiver_resume();
    return failed;
}

static void scan_hop(unsigned ch)
{
    phy_begin();
    phy_set_freq(rf_channel_freq(ch), 0);
    rf_force_gain(s_scan_gain);
    phy_end();
}

void scan_sweep(uint8_t *window, uint16_t power[SCAN_CHANNELS], uint32_t *no_data)
{
    for (unsigned k = 0; k < SCAN_CHANNELS; k++) {
        const unsigned ch = scan_order[k];
        scan_hop(ch);
        const int64_t fresh = esp_timer_get_time() + SCAN_FRESH_US;
        while (esp_timer_get_time() < fresh) {}
        if (copy_control_window(window, CONTROL_BYTES)) {
            power[ch] = (uint16_t)hs_window_power(window, CONTROL_BYTES);
        } else {
            power[ch] = SD_NO_DATA;
            (*no_data)++;
        }
    }
}
