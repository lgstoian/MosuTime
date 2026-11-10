/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The SD card is the source of truth, but the watch must still boot correctly
 * when the card is missing, dirty or being rewritten.  The last good config
 * is cached in NVS.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mosu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mosu_config_store_save(const mosu_config_t *cfg);
bool mosu_config_store_load(mosu_config_t *cfg);

/* Timezone discovered from the network, remembered across reboots. */
esp_err_t mosu_config_store_save_timezone(const char *tz);
bool mosu_config_store_load_timezone(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
