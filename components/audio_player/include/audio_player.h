/* Checkpoint audio: short sounds on a MAX98357A I2S amplifier.
 *
 * One low-priority task plays requests from a queue of at most two (a full
 * queue drops its oldest request). The I2S channel, and with it a GDMA TX
 * channel, exists only while sounds play: it is created before a sound and
 * deleted ~50 ms after the last one, so that a receiver transport restart
 * (which turns off every GDMA channel's interrupts) never leaves a stale
 * channel behind. Nothing is allocated while samples are being written.
 *
 * SD (amplifier enable) goes high just before a sound and low ~50 ms after
 * the last one. The PCM sits in flash. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_resample.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AUDIO_SOUND_COIN, AUDIO_SOUNDS } audio_sound_t;

#define AUDIO_SAMPLE_RATE_HZ 22050u
#define AUDIO_VOLUME_DEFAULT 60u   /* percent */

typedef struct {
    int bclk_gpio, ws_gpio, dout_gpio;
    int sd_gpio;                /* -1: SD not driven by the firmware */
    unsigned task_priority;     /* below every receiver task */
} audio_player_config_t;

typedef struct {
    uint32_t played;            /* sounds written in full */
    uint32_t dropped;           /* requests pushed out of a full queue */
    uint32_t muted;             /* requests refused while muted */
    uint32_t failed;            /* I2S open or write failures (sound cut) */
} audio_player_stats_t;

/* ESP_ERR_NOT_SUPPORTED when the build has no audio (CONFIG_AUDIO_PLAYER_ENABLE),
 * ESP_ERR_NO_MEM without memory for the task. */
esp_err_t audio_player_start(const audio_player_config_t *cfg);
bool audio_player_ready(void);
/* Non-blocking, from any task. pitch_q16: AUDIO_PITCH_ONE = recorded pitch. */
bool audio_player_play(audio_sound_t sound, uint32_t pitch_q16);
void audio_player_set_volume(unsigned percent);
unsigned audio_player_volume(void);
void audio_player_set_muted(bool muted);
bool audio_player_muted(void);
audio_player_stats_t audio_player_stats(void);

#ifdef __cplusplus
}
#endif
