/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The watch's state machine, separated from the watch itself.  This component
 * decides when the panel is lit, what a touch means, and which icon the status
 * bar should show.  It owns no hardware, no clock and no LVGL: the application
 * feeds it samples and timestamps, then acts on the answers.  That keeps the
 * behaviour testable on a PC, where a screen cannot be switched on.
 *
 * Nothing in mosu_session may include ESP-IDF headers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mosu_core/mosu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The displayed percentage is the mean of this many recent samples. */
#define MOSU_SESSION_BATTERY_SAMPLES 4

#define MOSU_SESSION_SCREEN_ON_MIN_MS 1000u
#define MOSU_SESSION_SCREEN_ON_MAX_MS 300000u
#define MOSU_SESSION_SCREEN_ON_DEFAULT_MS 15000u

/* Values match mosu_ui_wifi_t so the application can cast directly. */
typedef enum {
    MOSU_SESSION_WIFI_IDLE = 0,
    MOSU_SESSION_WIFI_OK = 1,
    MOSU_SESSION_WIFI_FAIL = 2,
} mosu_session_wifi_t;

/*
 * Numeric values match mosu_net_state_t.  The session does not include the
 * network component: the firmware casts its status to this small enum.
 */
typedef enum {
    MOSU_SESSION_NET_IDLE = 0,
    MOSU_SESSION_NET_CONNECTING = 1,
    MOSU_SESSION_NET_CONNECTED = 2,
    MOSU_SESSION_NET_FAILED = 3,
} mosu_session_net_state_t;

typedef struct {
    uint32_t screen_on_ms; /* how long the panel stays lit after activity */
    mosu_tap_cfg_t tap;
    uint16_t battery_green_min;
    uint16_t battery_orange_min;
} mosu_session_cfg_t;

typedef struct {
    mosu_session_cfg_t cfg;

    bool screen_on;
    uint32_t last_activity_ms;

    mosu_tap_t tap;
    bool say_pending;
    bool wake_pending;

    uint8_t battery_samples[MOSU_SESSION_BATTERY_SAMPLES];
    uint8_t battery_count;
    uint8_t battery_next;
    uint8_t battery_shown;
    bool battery_have;
} mosu_session_t;

/*
 * Initialise the session.  A NULL config, or an individual zero value, falls
 * back to the shipped default; every other value is clamped into its safe
 * range.  The watch starts with the screen off and no pending request.
 */
void mosu_session_init(mosu_session_t *session, const mosu_session_cfg_t *cfg);

/*
 * Feed one raw touch edge.  The double tap detector runs while the screen is
 * off too, so a gesture that starts in the dark still counts once the panel
 * wakes.  A double tap sets the pending "say" request; a press seen while the
 * screen is off also sets the pending wake request.
 */
void mosu_session_touch(mosu_session_t *session,
                        const mosu_touch_sample_t *sample);

/* Consume the pending announcement request.  True exactly once per gesture. */
bool mosu_session_take_say(mosu_session_t *session);

/* Consume the pending "a finger touched a dark screen" request. */
bool mosu_session_take_wake(mosu_session_t *session);

void mosu_session_screen_on(mosu_session_t *session, uint32_t now_ms);
void mosu_session_screen_off(mosu_session_t *session, uint32_t now_ms);

bool mosu_session_is_on(const mosu_session_t *session);

/* Refresh the inactivity deadline without changing the screen state. */
void mosu_session_activity(mosu_session_t *session, uint32_t now_ms);

/*
 * Decide whether the panel should go dark now.  Audio playback and a finger
 * resting on the glass both count as activity and push the deadline forward;
 * that is the only state this decision changes.
 *
 * Returns false when the screen is already off.
 */
bool mosu_session_should_sleep(mosu_session_t *session, uint32_t now_ms,
                               bool audio_busy, bool touch_active);

/*
 * Feed one battery reading and return the percentage that should be shown.
 * The first reading after boot, and any reading with immediate set, snaps to
 * the measured value; the others are averaged with the previous samples.
 * present == false clears the history and returns 0.
 */
uint8_t mosu_session_battery(mosu_session_t *session, uint8_t raw_percent,
                             bool present, bool immediate);

/* Colour bucket for the smoothed percentage, UNKNOWN before the first sample. */
mosu_batt_bucket_t mosu_session_battery_bucket(const mosu_session_t *session);

/*
 * Map the network state to the Wi-Fi icon.  A failed link always wins, even
 * when the time was synced successfully earlier.
 */
mosu_session_wifi_t
mosu_session_wifi_icon(mosu_session_net_state_t net_state, bool have_sync);

#ifdef __cplusplus
}
#endif
