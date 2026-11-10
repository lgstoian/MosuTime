/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include "esp_log.h"

#include "mosu_board.h"
#include "mosu_board_priv.h"

static const char *TAG = "mosu_board";

esp_err_t mosu_board_init(void)
{
    esp_err_t err = mosu_board_i2c_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mosu_board_pmu_init();
    if (err != ESP_OK) {
        /* A dead PMU still allows a demo, but no battery information. */
        ESP_LOGE(TAG, "PMU init failed: %s", esp_err_to_name(err));
    }

    err = mosu_board_rtc_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed: %s", esp_err_to_name(err));
    }

    err = mosu_board_imu_init();
    if (err != ESP_OK) {
        /* Wrist raise is a comfort feature; touch wake still works. */
        ESP_LOGW(TAG, "IMU init failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}
