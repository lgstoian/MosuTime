/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosu_config.h"
#include "mosu_core/mosu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mosu_audio_start(const mosu_config_t *cfg);

/* Queue an announcement.  A newer request replaces a waiting one. */
bool mosu_audio_say(const mosu_phrase_t *phrase);

bool mosu_audio_busy(void);
void mosu_audio_set_volume(uint8_t volume);

/* Audit: time the audio path has been active, and a kill switch for A/B runs. */
uint32_t mosu_audio_on_ms(void);
void mosu_audio_set_enabled(bool enable);
bool mosu_audio_enabled(void);

#ifdef __cplusplus
}
#endif
