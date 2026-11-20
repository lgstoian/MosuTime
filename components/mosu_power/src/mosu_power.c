/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"

#include "mosu_power.h"

static const char *TAG = "mosu_power";

static const mosu_hal_t *s_hal;
static esp_pm_lock_handle_t s_display_lock;
static uint8_t s_brightness = 55;
static bool s_screen_on;

esp_err_t mosu_power_init(const mosu_config_t *cfg)
{
    s_hal = mosu_hal();
    if (!s_hal) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg) {
        s_brightness = cfg->display.brightness;
    }

    /* Dynamic frequency: DFS scales between these two by load, on both cores.
     * Light sleep is entered whenever no lock is held, which is most of the
     * time while the face is off. */
    const esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&pm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }

    if (!s_display_lock &&
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "mosu_display",
                           &s_display_lock) != ESP_OK) {
        s_display_lock = NULL;
    }

    if (s_brightness < 1) {
        s_brightness = 1;
    }
    ESP_RETURN_ON_ERROR(s_hal->brightness_set(s_brightness), TAG, "bright");
    ESP_LOGI(TAG, "ready: max 160 MHz, min 80 MHz, light sleep on");
    return ESP_OK;
}

esp_err_t mosu_power_screen_on(void)
{
    if (!s_hal) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_screen_on) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(s_hal->display_on(), TAG, "display on");
    ESP_RETURN_ON_ERROR(s_hal->brightness_set(s_brightness), TAG, "bright");
    ESP_RETURN_ON_ERROR(s_hal->touch_wake(false), TAG, "touch");
    if (s_display_lock) {
        esp_pm_lock_acquire(s_display_lock);
    }
    s_screen_on = true;
    return ESP_OK;
}

esp_err_t mosu_power_screen_off(void)
{
    if (!s_hal) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_screen_on) {
        return ESP_OK;
    }
    if (s_display_lock) {
        esp_pm_lock_release(s_display_lock);
    }
    esp_err_t err = s_hal->display_off();
    s_screen_on = false;
    return err;
}

bool mosu_power_screen_is_on(void)
{
    return s_screen_on;
}

esp_err_t mosu_power_battery(mosu_hal_battery_t *out)
{
    if (!s_hal) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_hal->battery_read(out);
}

esp_err_t mosu_power_shutdown(void)
{
    if (!s_hal) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = s_hal->rtc_save();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC save: %s", esp_err_to_name(err));
    }
    s_hal->display_off();
    s_screen_on = false;
    return s_hal->power_off();
}
