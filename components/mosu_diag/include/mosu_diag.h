/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "mosu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*screen_on)(void);
    void (*screen_off)(void);
    /* Receives a mosu_ui_test_t value; diag does not depend on mosu_ui. */
    void (*test_pattern)(int pattern);
    void (*power_wifi)(void);
    void (*power_idle)(void);
} mosu_diag_hooks_t;

/*
 * A small line console on the USB/UART port.  It is for the person who
 * maintains the watch, never for the wearer: the watch face has no menus.
 */
esp_err_t mosu_diag_start(mosu_config_t *cfg);

/*
 * The console cannot switch the panel or the radio by itself: those belong to
 * app_main.  Hooks are optional; a missing one prints a note instead of
 * pretending the action happened.
 */
void mosu_diag_set_hooks(const mosu_diag_hooks_t *hooks);

/* True while "test touch" is armed: app_main logs every touch edge. */
bool mosu_diag_touch_test_active(void);

#ifdef __cplusplus
}
#endif
