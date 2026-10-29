/* Checkpoint pass detection and coin sound (checkpoint.h).
 *
 * A priority-1 task (under control/gain_obs at 3 and gain_fast at 4) reads
 * the C/N every 50 ms, feeds components/pass_detector with the tuned
 * channel's number in its band (R1..R8 -> 0..7) and, on a pass, prints a
 * PASS line and asks components/audio_player for the coin at that
 * channel's pitch. No sample while the receiver is not live (menu, scan,
 * calibration, paused) or the C/N is unknown (gain settling, no noise
 * reference, clipping): the detector then ages the channel out.
 *
 * Console (console.c): P plays the coin at the tuned channel's pitch, p
 * mutes/unmutes, K then one key per change tunes the detector and volume
 * (checkpoint_help). Settings are kept in NVS "checkpoint". */
#include "checkpoint.h"
#include "receiver.h"
#include <stdio.h>
#include <stdlib.h>
#include "audio_player.h"
#include "cn_estimate.h"
#include "esp_timer.h"
#include "pass_detector.h"
#include "rf.h"
#include "sdkconfig.h"

#define CHECKPOINT_TICK_MS   50u
#define CHECKPOINT_PRIORITY  1u     /* below every receiver task */
#define CHECKPOINT_STACK     3072u

/* NVS "checkpoint"; a new layout gets a new version (old blobs ignored). */
#define PREFS_VERSION 1u
typedef struct {
    uint8_t version, volume, muted, reserved;
    pd_config_t det;
} checkpoint_prefs_t;

typedef struct {
    pass_detector_t det;
    pd_config_t wanted;              /* console copy; the task applies it */
    volatile bool wanted_dirty, trace;
    uint32_t valid, unknown, passes;
    bool has_last;
    pd_event_t last;
    const char *last_name;           /* its channel, e.g. "R3" */
} checkpoint_t;

static checkpoint_t *s_cp;
static bool s_audio_refused;
static portMUX_TYPE s_wanted_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

#if CONFIG_AUDIO_PLAYER_ENABLE
#ifdef CONFIG_AUDIO_PLAYER_ALLOW_RESERVED_DAC_PINS
#define RESERVED_DAC_PINS_ALLOWED 1
#else
#define RESERVED_DAC_PINS_ALLOWED 0
#endif

/* The receiver owns these pins (rf.h, transport.c, board); an amplifier on
 * one of them would corrupt the IQ or the video. */
static const char *pin_owner(int gpio)
{
    static const int iq[8] = RF_IQ_GPIO_NUMS;
    static const int dac[6] = {23, 24, 11, 12, 8, 9};   /* transport.c s_dac_gpio */
    for (unsigned k = 0; k < 8u; ++k) if (iq[k] == gpio) return "IQ loopback";
    for (unsigned k = 0; k < 6u; ++k) if (dac[k] == gpio) return "video DAC";
    if ((gpio == 7 || gpio == 10) && !RESERVED_DAC_PINS_ALLOWED) return "reserved DAC branch";
    if (gpio == 13 || gpio == 14) return "USB";
    if (gpio >= 16 && gpio <= 22) return "flash";
    if (gpio == 26) return "antenna switch";
    if (gpio == 28) return "BOOT button";
    return NULL;
}

static void audio_start(void)
{
    const audio_player_config_t cfg = {
        .bclk_gpio = CONFIG_AUDIO_PLAYER_BCLK_GPIO, .ws_gpio = CONFIG_AUDIO_PLAYER_WS_GPIO,
        .dout_gpio = CONFIG_AUDIO_PLAYER_DOUT_GPIO, .sd_gpio = CONFIG_AUDIO_PLAYER_SD_GPIO,
        .task_priority = CHECKPOINT_PRIORITY,
    };
    const int pins[4] = {cfg.bclk_gpio, cfg.ws_gpio, cfg.dout_gpio, cfg.sd_gpio};
    static const char *const names[4] = {"BCLK", "LRC", "DIN", "SD"};
    for (unsigned k = 0; k < 4u; ++k) {
        const char *owner = pin_owner(pins[k]);
        for (unsigned j = 0; !owner && j < k; ++j) if (pins[j] >= 0 && pins[j] == pins[k]) owner = names[j];
        if (owner) {
            printf("CHECKPOINT audio=refused pin=%s gpio=%d owner=\"%s\" (menuconfig: Checkpoint audio)\n", names[k],
                   pins[k], owner);
            s_audio_refused = true;
            return;
        }
    }
    const esp_err_t err = audio_player_start(&cfg);
    if (err != ESP_OK) {
        printf("CHECKPOINT audio=failed %s\n", esp_err_to_name(err));
        s_audio_refused = true;
    }
}
#else
static void audio_start(void) {}
#endif

static const char *audio_state(void)
{
    return audio_player_ready() ? (audio_player_muted() ? "muted" : "on") : s_audio_refused ? "refused" : "off_build";
}

static void on_pass(checkpoint_t *cp, const pd_event_t *e)
{
    const fpv_channel_t *ch = rf_channel();
    ++cp->passes;
    cp->last = *e;
    cp->last_name = ch->name;
    cp->has_last = true;
    const bool queued = audio_player_play(AUDIO_SOUND_COIN, audio_pitch_for_channel(e->channel));
    printf("PASS channel=%s freq=%u peak_cn_db=%.1f peak_ms=%lu at_ms=%lu delay_ms=%lu semitones=%+d audio=%s\n",
           ch->name, ch->freq_mhz, e->peak_db, (unsigned long)e->peak_ms, (unsigned long)e->at_ms,
           (unsigned long)(e->at_ms - e->peak_ms), audio_pentatonic_semitones[e->channel % 8u],
           queued ? "queued" : audio_state());
}

/* A live receiver sample, in dB; false when there is none. */
static bool sample_cn(float *db)
{
    if (s_menu_active || s_scan_active || menu_status_active() || receiver_paused() || !receiver_active()) return false;
    const int x10 = receiver_obs_cn_x10();
    if (x10 == CN_UNKNOWN_X10) return false;
    *db = (float)x10 / 10.0f;
    return true;
}

static void checkpoint_task(void *arg)
{
    checkpoint_t *cp = arg;
    uint32_t seen_epoch = s_control_epoch;
    size_t seen_index = rf_channel_index();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECKPOINT_TICK_MS));
        const uint32_t now = now_ms();
        const size_t index = rf_channel_index();
        /* Retune, scan or calibration: whatever was in progress is not a pass. */
        if (s_control_epoch != seen_epoch || index != seen_index) {
            seen_epoch = s_control_epoch;
            seen_index = index;
            pd_reset_all(&cp->det);
        }
        if (cp->wanted_dirty) {
            pd_config_t c;
            taskENTER_CRITICAL(&s_wanted_lock);
            c = cp->wanted;
            cp->wanted_dirty = false;
            taskEXIT_CRITICAL(&s_wanted_lock);
            (void)pd_set_config(&cp->det, &c);   /* validated by the console */
        }
        const unsigned slot = (unsigned)(index % 8u);
        float db;
        if (!sample_cn(&db)) {
            ++cp->unknown;
            pd_expire(&cp->det, now);
            if (cp->trace) printf("CP_TRACE t_ms=%lu ch=%s cn_db=unknown state=%s\n", (unsigned long)now,
                                  rf_channel()->name, pd_state_name(cp->det.ch[slot].state));
            continue;
        }
        ++cp->valid;
        pd_event_t e;
        const bool pass = pd_update(&cp->det, slot, db, now, &e);
        if (cp->trace) printf("CP_TRACE t_ms=%lu ch=%s cn_db=%.1f smooth_db=%.1f peak_db=%.1f state=%s\n",
                              (unsigned long)now, rf_channel()->name, db, cp->det.ch[slot].smooth,
                              cp->det.ch[slot].peak, pd_state_name(cp->det.ch[slot].state));
        if (pass) on_pass(cp, &e);
    }
}

void checkpoint_start(void)
{
    if (s_cp) return;
    checkpoint_t *cp = calloc(1, sizeof(*cp));
    if (!cp) { printf("CHECKPOINT disabled: no memory\n"); return; }
    checkpoint_prefs_t prefs;
    const bool saved = blob_load("checkpoint", &prefs, sizeof(prefs)) && prefs.version == PREFS_VERSION &&
                       pd_config_valid(&prefs.det);
    const pd_config_t cfg = saved ? prefs.det : pd_config_default();
    pd_init(&cp->det, &cfg);
    cp->wanted = cp->det.cfg;
    if (saved) {
        audio_player_set_volume(prefs.volume);
        audio_player_set_muted(prefs.muted != 0);
    }
    audio_start();
    if (xTaskCreate(checkpoint_task, "checkpoint", CHECKPOINT_STACK, cp, CHECKPOINT_PRIORITY, NULL) != pdPASS) {
        free(cp);
        printf("CHECKPOINT disabled: no memory for the task\n");
        return;
    }
    s_cp = cp;
}

void checkpoint_print(void)
{
    checkpoint_t *cp = s_cp;
    if (!cp) { printf("CHECKPOINT off\n"); return; }
    const pd_config_t *c = &cp->det.cfg;
    const unsigned slot = (unsigned)(rf_channel_index() % 8u);
    const audio_player_stats_t a = audio_player_stats();
    printf("CHECKPOINT source=cn state=%s smooth_db=%.1f peak_db=%.1f samples_valid=%lu unknown=%lu passes=%lu "
           "alpha=%.2f active_db=%.1f hyst_db=%.1f drop_db=%.1f min_peak_db=%.1f refractory_ms=%lu stale_ms=%lu "
           "audio=%s volume=%u played=%lu dropped=%lu muted=%lu failed=%lu\n",
           pd_state_name(cp->det.ch[slot].state), cp->det.ch[slot].smooth, cp->det.ch[slot].peak,
           (unsigned long)cp->valid, (unsigned long)cp->unknown, (unsigned long)cp->passes, c->alpha, c->active_db,
           c->hysteresis_db, c->drop_db, c->min_peak_db, (unsigned long)c->refractory_ms, (unsigned long)c->stale_ms,
           audio_state(), audio_player_volume(), (unsigned long)a.played, (unsigned long)a.dropped,
           (unsigned long)a.muted, (unsigned long)a.failed);
    if (cp->has_last)
        printf("CHECKPOINT last_pass channel=%s peak_cn_db=%.1f at_ms=%lu\n", cp->last_name,
               cp->last.peak_db, (unsigned long)cp->last.at_ms);
}

static void prefs_save(const checkpoint_t *cp)
{
    const checkpoint_prefs_t p = {.version = PREFS_VERSION, .volume = (uint8_t)audio_player_volume(),
                                  .muted = audio_player_muted(), .det = cp->wanted};
    if (!blob_store("checkpoint", &p, sizeof(p))) printf("CHECKPOINT warning: settings not saved (NVS)\n");
}

void checkpoint_play_test(void)
{
    const fpv_channel_t *ch = rf_channel();
    const unsigned slot = (unsigned)(rf_channel_index() % 8u);
    const bool queued = audio_player_play(AUDIO_SOUND_COIN, audio_pitch_for_channel(slot));
    printf("CHECKPOINT play channel=%s semitones=%+d volume=%u audio=%s\n", ch->name,
           audio_pentatonic_semitones[slot], audio_player_volume(), queued ? "queued" : audio_state());
}

void checkpoint_toggle_mute(void)
{
    audio_player_set_muted(!audio_player_muted());
    if (s_cp) prefs_save(s_cp);
    printf("CHECKPOINT sound %s (saved)%s\n", audio_player_muted() ? "MUTED" : "ON",
           audio_player_ready() ? "" : "; this build has no audio output, PASS lines only");
}

void checkpoint_help(void)
{
    printf("CHECKPOINT K + key (lower = down, upper = up; K stays open 5 s after each key, q closes):\n"
           "  a/A smoothing alpha -/+0.05   t/T activity threshold -/+1 dB   h/H hysteresis -/+0.5 dB\n"
           "  d/D pass drop -/+0.5 dB        m/M minimum peak -/+1 dB         r/R refractory -/+250 ms\n"
           "  v/V volume -/+10 %%            l live C/N trace on/off (20 lines/s)   0 defaults   k show\n"
           "  Top level: P play the coin at this channel's pitch, p sound off/on.\n");
    checkpoint_print();
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

bool checkpoint_key(char key)
{
    checkpoint_t *cp = s_cp;
    if (!cp) return false;   /* checkpoint_help already said "CHECKPOINT off" */
    pd_config_t c = cp->wanted;
    int volume = (int)audio_player_volume();
    switch (key) {
    case 'a': case 'A': c.alpha = clampf(c.alpha + (key == 'A' ? 0.05f : -0.05f), 0.05f, 1.0f); break;
    case 't': case 'T': c.active_db += key == 'T' ? 1.0f : -1.0f; break;
    case 'h': case 'H': c.hysteresis_db = clampf(c.hysteresis_db + (key == 'H' ? 0.5f : -0.5f), 0.5f, 20.0f); break;
    case 'd': case 'D': c.drop_db = clampf(c.drop_db + (key == 'D' ? 0.5f : -0.5f), 0.5f, 20.0f); break;
    case 'm': case 'M': c.min_peak_db += key == 'M' ? 1.0f : -1.0f; break;
    case 'r': case 'R': {
        const int32_t ms = (int32_t)c.refractory_ms + (key == 'R' ? 250 : -250);
        c.refractory_ms = (uint32_t)(ms < 0 ? 0 : ms > 30000 ? 30000 : ms);
        break;
    }
    case 'v': case 'V': volume += key == 'V' ? 10 : -10; break;
    case 'l':
        cp->trace = !cp->trace;
        printf("CHECKPOINT trace %s\n", cp->trace ? "ON" : "OFF");
        return true;
    case '0': c = pd_config_default(); volume = AUDIO_VOLUME_DEFAULT; break;
    case 'k': case '?': checkpoint_print(); return true;
    default: return false;
    }
    if (!pd_config_valid(&c)) {
        printf("CHECKPOINT refused: minimum peak %.1f dB must stay at or above the activity threshold %.1f dB\n",
               c.min_peak_db, c.active_db);
        return true;
    }
    audio_player_set_volume((unsigned)(volume < 0 ? 0 : volume));
    taskENTER_CRITICAL(&s_wanted_lock);
    cp->wanted = c;
    cp->wanted_dirty = true;
    taskEXIT_CRITICAL(&s_wanted_lock);
    prefs_save(cp);
    printf("CHECKPOINT set alpha=%.2f active_db=%.1f hyst_db=%.1f drop_db=%.1f min_peak_db=%.1f refractory_ms=%lu "
           "volume=%u (saved)\n", c.alpha, c.active_db, c.hysteresis_db, c.drop_db, c.min_peak_db,
           (unsigned long)c.refractory_ms, audio_player_volume());
    return true;
}
