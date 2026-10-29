/* Checkpoint audio player (audio_player.h). */
#include "audio_player.h"
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static volatile uint8_t s_volume = AUDIO_VOLUME_DEFAULT;
static volatile bool s_muted;
static audio_player_stats_t s_stats;

void audio_player_set_volume(unsigned percent) { s_volume = (uint8_t)(percent > 100u ? 100u : percent); }
unsigned audio_player_volume(void) { return s_volume; }
void audio_player_set_muted(bool muted) { s_muted = muted; }
bool audio_player_muted(void) { return s_muted; }
audio_player_stats_t audio_player_stats(void) { return s_stats; }

#if !CONFIG_AUDIO_PLAYER_ENABLE

esp_err_t audio_player_start(const audio_player_config_t *cfg) { (void)cfg; return ESP_ERR_NOT_SUPPORTED; }
bool audio_player_ready(void) { return false; }
bool audio_player_play(audio_sound_t sound, uint32_t pitch_q16) { (void)sound; (void)pitch_q16; return false; }

#else

#include "driver/gpio.h"
#include "driver/i2s_std.h"

#define QUEUE_DEPTH     2u
#define TASK_STACK      3072u
#define DMA_DESCS       4u
#define DMA_FRAMES      200u            /* 4 x 200 mono samples: 36 ms, 1.6 KB */
#define CHUNK           128u            /* resampler output per write */
#define WRITE_TIMEOUT_MS 200u           /* a stalled GDMA (transport restart) cuts the sound */
#define AMP_WAKE_MS     5u              /* SD high -> first sample */
#define AMP_HOLD_MS     50u             /* last sample -> SD low */

extern const uint8_t coin_pcm_start[] asm("_binary_coin_pcm_start");
extern const uint8_t coin_pcm_end[] asm("_binary_coin_pcm_end");

typedef struct { uint8_t sound; uint32_t pitch_q16; } request_t;

static audio_player_config_t s_cfg;
static QueueHandle_t s_queue;
static i2s_chan_handle_t s_tx;
static const int16_t s_silence[DMA_DESCS * DMA_FRAMES];   /* pushes the last sound out of the DMA ring */

static void amp_enable(bool on)
{
    if (s_cfg.sd_gpio >= 0) gpio_set_level((gpio_num_t)s_cfg.sd_gpio, on);
}

/* A fresh channel per burst of sounds: it also (re)enables this channel's
 * GDMA interrupts, which a receiver transport restart turns off. */
static bool open_channel(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan.dma_desc_num = DMA_DESCS;
    chan.dma_frame_num = DMA_FRAMES;
    chan.auto_clear_after_cb = true;      /* an underrun plays silence, not the old buffer */
    const i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)s_cfg.bclk_gpio,
            .ws = (gpio_num_t)s_cfg.ws_gpio,
            .dout = (gpio_num_t)s_cfg.dout_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    esp_err_t err = i2s_new_channel(&chan, &s_tx, NULL);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_tx, &std);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) return true;
    printf("AUDIO error=open %s\n", esp_err_to_name(err));
    if (s_tx) (void)i2s_del_channel(s_tx);
    s_tx = NULL;
    return false;
}

static void close_channel(void)
{
    if (!s_tx) return;
    (void)i2s_channel_disable(s_tx);
    (void)i2s_del_channel(s_tx);
    s_tx = NULL;
}

static bool write_all(const void *data, size_t bytes)
{
    size_t done = 0;
    esp_err_t err = i2s_channel_write(s_tx, data, bytes, &done, WRITE_TIMEOUT_MS);
    if (err == ESP_OK && done == bytes) return true;
    printf("AUDIO error=write %s written=%u/%u\n", esp_err_to_name(err), (unsigned)done, (unsigned)bytes);
    return false;
}

static bool play(const request_t *r)
{
    const uint8_t *pcm = coin_pcm_start;
    const uint32_t samples = (uint32_t)(coin_pcm_end - coin_pcm_start) / 2u;
    int16_t chunk[CHUNK];   /* on the task stack: no static RAM in the boot peak */
    audio_voice_t v;
    audio_voice_start(&v, pcm, samples, r->pitch_q16, audio_gain_q15(s_volume));
    for (size_t n; (n = audio_voice_render(&v, chunk, CHUNK)) != 0;)
        if (!write_all(chunk, n * sizeof(chunk[0]))) return false;
    return true;
}

static void audio_task(void *arg)
{
    (void)arg;
    request_t r;
    for (;;) {
        (void)xQueueReceive(s_queue, &r, portMAX_DELAY);
        if (s_muted) continue;
        if (!open_channel()) { ++s_stats.failed; continue; }
        amp_enable(true);
        vTaskDelay(pdMS_TO_TICKS(AMP_WAKE_MS));
        bool ok = true;
        do {
            ok = !s_muted && play(&r);
            if (!ok) break;
            ++s_stats.played;
        } while (xQueueReceive(s_queue, &r, 0) == pdTRUE);
        /* Let the ring drain, then hold the amplifier on a little: a request
         * in that window plays at once on the same channel. */
        if (ok) ok = write_all(s_silence, sizeof(s_silence));
        while (ok && xQueueReceive(s_queue, &r, pdMS_TO_TICKS(AMP_HOLD_MS)) == pdTRUE) {
            ok = !s_muted && play(&r) && write_all(s_silence, sizeof(s_silence));
            if (ok) ++s_stats.played;
        }
        if (!ok && !s_muted) ++s_stats.failed;
        amp_enable(false);
        close_channel();
    }
}

static bool valid_gpio(int gpio) { return gpio >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(gpio); }

esp_err_t audio_player_start(const audio_player_config_t *cfg)
{
    if (s_queue) return ESP_ERR_INVALID_STATE;
    if (!cfg || !valid_gpio(cfg->bclk_gpio) || !valid_gpio(cfg->ws_gpio) || !valid_gpio(cfg->dout_gpio) ||
        (cfg->sd_gpio >= 0 && !valid_gpio(cfg->sd_gpio)) || coin_pcm_end - coin_pcm_start < 2)
        return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    if (s_cfg.sd_gpio >= 0) {
        const gpio_config_t sd = {.pin_bit_mask = 1ULL << s_cfg.sd_gpio, .mode = GPIO_MODE_OUTPUT};
        esp_err_t err = gpio_config(&sd);
        if (err != ESP_OK) return err;
        amp_enable(false);
    }
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(request_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(audio_task, "audio", TASK_STACK, NULL, s_cfg.task_priority, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool audio_player_ready(void) { return s_queue != NULL; }

bool audio_player_play(audio_sound_t sound, uint32_t pitch_q16)
{
    if (!s_queue || sound >= AUDIO_SOUNDS) return false;
    if (s_muted) { ++s_stats.muted; return false; }
    const request_t r = {(uint8_t)sound, pitch_q16};
    if (xQueueSend(s_queue, &r, 0) == pdTRUE) return true;
    request_t old;
    if (xQueueReceive(s_queue, &old, 0) == pdTRUE) ++s_stats.dropped;   /* keep the newest two */
    return xQueueSend(s_queue, &r, 0) == pdTRUE;
}

#endif
