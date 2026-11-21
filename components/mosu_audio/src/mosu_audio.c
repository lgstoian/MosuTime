/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Announcement playback.
 *
 * The clips are tiny mono WAV files on the SD card; the announcement is built
 * at runtime by playing h_N.wav, then m_N.wav, then d_N.wav with a small gap.
 * Nothing is embedded in flash, so the voice can be replaced by copying new
 * files onto the card.
 *
 * While a sentence is playing we hold a NO_LIGHT_SLEEP power lock: the audio
 * path is not safe across a light sleep, and the sentence is short.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mosu_audio.h"
#include "mosu_board.h"
#include "mosu_tasks.h"

static const char *TAG = "mosu_audio";

static const mosu_config_t *s_cfg;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static esp_pm_lock_handle_t s_pm_lock;
static volatile bool s_busy;
static volatile uint8_t s_volume;
/* Audit: time the codec/PA path has been active. */
static uint32_t s_audio_on_ms;
static uint32_t s_audio_on_since;
static volatile bool s_disabled;

typedef struct {
    mosu_phrase_t phrase;
} audio_request_t;

static void audio_task(void *arg);

esp_err_t mosu_audio_start(const mosu_config_t *cfg)
{
    if (s_task) {
        return ESP_OK;
    }
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;
    s_volume = cfg->announce.volume;

    s_queue = xQueueCreate(1, sizeof(audio_request_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    /* Best effort: without PM enabled the lock is simply unused. */
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "mosu_say", &s_pm_lock) !=
        ESP_OK) {
        s_pm_lock = NULL;
    }

    if (mosu_task_start(&(mosu_task_cfg_t){
                             .name = "mosu_audio",
                             .fn = audio_task,
                             .stack = 6144,
                             .priority = 6,
                             .core = tskNO_AFFINITY,
                         },
                         &s_task) != ESP_OK) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "voice clips in %s", cfg->announce.voice_dir);
    return ESP_OK;
}

static void play_clip(const mosu_clip_t *clip)
{
    char name[24];
    char path[MOSU_CFG_PATH_MAX + 32];

    if (!mosu_clip_filename(clip->kind, clip->index, name, sizeof(name))) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", s_cfg->announce.voice_dir, name);

    esp_err_t err = mosu_board_audio_play_wav(path, s_volume);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "%s: %s", path, esp_err_to_name(err));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    audio_request_t request;

    for (;;) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_busy = true;
        s_audio_on_since = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_pm_lock) {
            esp_pm_lock_acquire(s_pm_lock);
        }

        /* Creating the codec is slow; do it while the power lock is held so
         * a light sleep cannot interrupt the I2S setup. */
        mosu_board_audio_init();

        for (uint8_t i = 0; i < request.phrase.count; i++) {
            play_clip(&request.phrase.clips[i]);
            if (i + 1 < request.phrase.count && s_cfg->announce.clip_gap_ms) {
                vTaskDelay(pdMS_TO_TICKS(s_cfg->announce.clip_gap_ms));
            }
        }

        mosu_board_audio_idle();

        if (s_audio_on_since) {
            s_audio_on_ms +=
                (uint32_t)(esp_timer_get_time() / 1000) - s_audio_on_since;
            s_audio_on_since = 0;
        }
        if (s_pm_lock) {
            esp_pm_lock_release(s_pm_lock);
        }
        s_busy = false;
    }
}

bool mosu_audio_say(const mosu_phrase_t *phrase)
{
    if (s_disabled || !s_task || !phrase || !phrase->valid ||
        phrase->count == 0) {
        return false;
    }

    audio_request_t request = {.phrase = *phrase};

    /* A newer request replaces one that is still waiting; it never cuts a
     * sentence that is already being spoken. */
    if (xQueueOverwrite(s_queue, &request) != pdTRUE) {
        return false;
    }
    return true;
}

bool mosu_audio_busy(void)
{
    return s_busy;
}

void mosu_audio_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
}

uint32_t mosu_audio_on_ms(void)
{
    uint32_t ms = s_audio_on_ms;
    if (s_audio_on_since) {
        ms += (uint32_t)(esp_timer_get_time() / 1000) - s_audio_on_since;
    }
    return ms;
}

void mosu_audio_set_enabled(bool enable)
{
    s_disabled = !enable;
    if (!enable) {
        mosu_board_audio_idle();
    }
}

bool mosu_audio_enabled(void)
{
    return !s_disabled;
}
