/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The board side of the basic HAL.  Every pointer here is a thin call into
 * the board component, so a port implements one table and nothing else.
 */
#include <time.h>

#include "esp_check.h"

#include "mosu_board.h"
#include "mosu_hal.h"

static const char *TAG = "mosu_hal";

static esp_err_t hal_display_on(void)
{
    return mosu_board_display_on();
}

static esp_err_t hal_display_off(void)
{
    return mosu_board_display_off();
}

static esp_err_t hal_brightness(uint8_t percent)
{
    return mosu_board_brightness_set(percent);
}

static esp_err_t hal_touch_wake(bool enable)
{
    return mosu_board_enable_touch_wake(enable);
}

static esp_err_t hal_battery(mosu_hal_battery_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    mosu_board_pmu_t pmu;
    ESP_RETURN_ON_ERROR(mosu_board_pmu_read(&pmu), TAG, "pmu");
    out->millivolts = pmu.millivolts;
    out->percent = pmu.percent;
    out->present = pmu.battery_present;
    out->charging = pmu.charging;
    out->vbus = pmu.vbus_present;
    return ESP_OK;
}

static esp_err_t hal_power_off(void)
{
    return mosu_board_pmu_power_off();
}

static esp_err_t hal_rtc_save(void)
{
    const time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    return mosu_board_rtc_write(&utc);
}

static const mosu_hal_t s_hal = {
    .display_on = hal_display_on,
    .display_off = hal_display_off,
    .brightness_set = hal_brightness,
    .touch_wake = hal_touch_wake,
    .battery_read = hal_battery,
    .power_off = hal_power_off,
    .rtc_save = hal_rtc_save,
};

const mosu_hal_t *mosu_hal(void)
{
    return &s_hal;
}
