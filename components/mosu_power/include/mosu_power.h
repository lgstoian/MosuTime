/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Dedicated power manager.
 *
 * One owner for the things that decide how much current the watch draws:
 * the panel state, its brightness, the no-light-sleep lock while the face is
 * lit, the CPU/DFS configuration, the external RTC write at shutdown and the
 * PMU power-off.  The session state machine still decides *when* the face
 * should be on; this component decides *how* that is done.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mosu_config.h"
#include "mosu_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apply the display brightness and the CPU/light-sleep policy.  Call after
 * the board and display are initialised. */
esp_err_t mosu_power_init(const mosu_config_t *cfg);

/* Panel on, brightness restored, no-light-sleep held.  Safe to call when
 * already on. */
esp_err_t mosu_power_screen_on(void);

/* Down to brightness zero, panel off, lock released. */
esp_err_t mosu_power_screen_off(void);

bool mosu_power_screen_is_on(void);

/* Battery and charger state through the HAL. */
esp_err_t mosu_power_battery(mosu_hal_battery_t *out);

/* Save the clock to the RTC, switch the panel off and cut the rails.  Does
 * not return when the PMU accepts it. */
esp_err_t mosu_power_shutdown(void);

#ifdef __cplusplus
}
#endif
