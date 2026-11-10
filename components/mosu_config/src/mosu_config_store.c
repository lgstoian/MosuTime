/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "mosu_config_store.h"

static const char *TAG = "mosu_cfg";
static const char *NS = "mosu";
static const char *KEY_CFG = "cfg";
static const char *KEY_TZ = "tz";
static const uint32_t CFG_MAGIC = 0x4D555331; /* "MUS1" */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
} store_header_t;

esp_err_t mosu_config_store_save(const mosu_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    store_header_t header = {
        .magic = CFG_MAGIC,
        .version = 1,
        .size = sizeof(*cfg),
    };

    /* Single blob: one write, one erase, no partial config. */
    size_t blob_len = sizeof(header) + sizeof(*cfg);
    uint8_t *blob = malloc(blob_len);
    if (!blob) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    memcpy(blob, &header, sizeof(header));
    memcpy(blob + sizeof(header), cfg, sizeof(*cfg));

    err = nvs_set_blob(handle, KEY_CFG, blob, blob_len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    free(blob);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to cache config: %s", esp_err_to_name(err));
    }
    return err;
}

bool mosu_config_store_load(mosu_config_t *cfg)
{
    if (!cfg) {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    store_header_t header;
    size_t blob_len = 0;
    esp_err_t err = nvs_get_blob(handle, KEY_CFG, NULL, &blob_len);
    if (err != ESP_OK || blob_len != sizeof(header) + sizeof(*cfg)) {
        nvs_close(handle);
        return false;
    }

    uint8_t *blob = malloc(blob_len);
    if (!blob) {
        nvs_close(handle);
        return false;
    }
    err = nvs_get_blob(handle, KEY_CFG, blob, &blob_len);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(blob);
        return false;
    }

    memcpy(&header, blob, sizeof(header));
    if (header.magic != CFG_MAGIC || header.version != 1 ||
        header.size != sizeof(*cfg)) {
        free(blob);
        return false;
    }

    memcpy(cfg, blob + sizeof(header), sizeof(*cfg));
    free(blob);
    mosu_config_sanitize(cfg);
    return true;
}

esp_err_t mosu_config_store_save_timezone(const char *tz)
{
    if (!tz || !mosu_config_timezone_ok(tz)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, KEY_TZ, tz);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool mosu_config_store_load_timezone(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }

    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t required = len;
    esp_err_t err = nvs_get_str(handle, KEY_TZ, buf, &required);
    nvs_close(handle);
    return err == ESP_OK && mosu_config_timezone_ok(buf);
}
