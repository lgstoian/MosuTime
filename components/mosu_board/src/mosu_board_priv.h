/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

i2c_master_bus_handle_t mosu_board_i2c_bus(void);
esp_err_t mosu_board_i2c_start(void);

esp_err_t mosu_board_pmu_init(void);
esp_err_t mosu_board_rtc_init(void);
esp_err_t mosu_board_imu_init(void);
