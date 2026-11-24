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
    uint32_t epoch;
    uint32_t start_epoch;
    uint16_t mv;
    uint8_t pct;
    uint8_t start_pct;
} mosu_power_log_t;

typedef struct {
    uint32_t uptime_s;
    uint32_t screen_on_s;
    uint32_t wakes;
    uint32_t boots;
    uint16_t battery_mv;
    uint16_t screen_ms;
    uint16_t hold_ms;
    uint16_t face_milli;
    uint32_t raise_fired;
    uint32_t raise_candidates;
    uint32_t wifi_on_s;
    uint32_t audio_on_s;
    /* Light-sleep entries, kept in RTC no-init memory across console resets. */
    uint32_t sleep_entries;
    int16_t last_nz_milli;
    uint8_t battery_pct;
    uint8_t boot_pct;
    uint8_t angle_deg;
    uint8_t last_raise_deg;
    bool battery_present;
    bool charging;
} mosu_diag_stats_t;

typedef struct {
    /* Wake the face for one normal activity window without forcing it on. */
    void (*wake)(void);
    void (*screen_on)(void);
    void (*screen_off)(void);
    /* Receives a mosu_ui_test_t value; diag does not depend on mosu_ui. */
    void (*test_pattern)(int pattern);
    void (*power_wifi)(void);
    void (*power_idle)(void);
    /* Fills the power accounting: uptime, screen-on seconds, wakes, battery. */
    void (*stats)(mosu_diag_stats_t *out);
    /* Start a new measurement window at the current battery percentage. */
    void (*stats_reset)(void);
    /* Audit switches and status: receives the arguments after "audit". */
    void (*audit)(const char *args);
    /* Last battery record written to flash; survives a dead cell. */
    bool (*power_log)(mosu_power_log_t *out);
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
