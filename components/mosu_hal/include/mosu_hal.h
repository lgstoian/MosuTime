/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Basic hardware abstraction for the parts the power manager needs.  The
 * board component is the single implementation; a port provides another one.
 * Keeping the table small is deliberate: an abstraction that mirrors every
 * vendor call only adds indirection, while the panel, PMU and RTC are the
 * pieces a different board actually changes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t millivolts;
    uint8_t percent;
    bool present;
    bool charging;
    bool vbus;
} mosu_hal_battery_t;

typedef struct {
    /* Display, brightness and the touch wake source. */
    esp_err_t (*display_on)(void);
    esp_err_t (*display_off)(void);
    esp_err_t (*brightness_set)(uint8_t percent);
    esp_err_t (*touch_wake)(bool enable);

    /* Battery and charger state. */
    esp_err_t (*battery_read)(mosu_hal_battery_t *out);

    /* Cut the rails; the caller has already saved the clock. */
    esp_err_t (*power_off)(void);

    /* Write the current system time to the external RTC. */
    esp_err_t (*rtc_save)(void);
} mosu_hal_t;

/* The board's implementation.  Never NULL after mosu_board_init(). */
const mosu_hal_t *mosu_hal(void);

#ifdef __cplusplus
}
#endif
