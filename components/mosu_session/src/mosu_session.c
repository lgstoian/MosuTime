/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Pure state for the watch face.  No ESP-IDF, no clock: every function takes
 * the timestamp the application already has, so the same code runs on the
 * watch and inside the host tests.
 */
#include <string.h>

#include "mosu_session.h"

#define TAP_DOUBLE_TAP_MIN_MS 200u
#define TAP_DOUBLE_TAP_MAX_MS 1500u
#define TAP_DOUBLE_TAP_DEFAULT_MS 650u

#define TAP_MAX_HOLD_MIN_MS 80u
#define TAP_MAX_HOLD_MAX_MS 1500u
#define TAP_MAX_HOLD_DEFAULT_MS 400u

#define TAP_MAX_MOVE_MIN_PX 4u
#define TAP_MAX_MOVE_MAX_PX 200u
#define TAP_MAX_MOVE_DEFAULT_PX 40u

#define TAP_COOLDOWN_MIN_MS 300u
#define TAP_COOLDOWN_MAX_MS 10000u
#define TAP_COOLDOWN_DEFAULT_MS 2000u

#define BATTERY_GREEN_DEFAULT 40u
#define BATTERY_ORANGE_DEFAULT 15u

static uint32_t clamp_u32(uint32_t value, uint32_t lo, uint32_t hi,
                          uint32_t fallback)
{
    if (value == 0) {
        return fallback;
    }
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static uint16_t clamp_u16(uint16_t value, uint16_t lo, uint16_t hi,
                          uint16_t fallback)
{
    if (value == 0) {
        return fallback;
    }
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

void mosu_session_init(mosu_session_t *session, const mosu_session_cfg_t *cfg)
{
    if (!session) {
        return;
    }
    memset(session, 0, sizeof(*session));

    session->cfg.screen_on_ms =
        clamp_u32(cfg ? cfg->screen_on_ms : 0, MOSU_SESSION_SCREEN_ON_MIN_MS,
                  MOSU_SESSION_SCREEN_ON_MAX_MS,
                  MOSU_SESSION_SCREEN_ON_DEFAULT_MS);

    session->cfg.tap.double_tap_ms =
        clamp_u16(cfg ? cfg->tap.double_tap_ms : 0, TAP_DOUBLE_TAP_MIN_MS,
                  TAP_DOUBLE_TAP_MAX_MS, TAP_DOUBLE_TAP_DEFAULT_MS);
    session->cfg.tap.max_hold_ms =
        clamp_u16(cfg ? cfg->tap.max_hold_ms : 0, TAP_MAX_HOLD_MIN_MS,
                  TAP_MAX_HOLD_MAX_MS, TAP_MAX_HOLD_DEFAULT_MS);
    session->cfg.tap.max_move_px =
        clamp_u16(cfg ? cfg->tap.max_move_px : 0, TAP_MAX_MOVE_MIN_PX,
                  TAP_MAX_MOVE_MAX_PX, TAP_MAX_MOVE_DEFAULT_PX);
    session->cfg.tap.cooldown_ms =
        clamp_u16(cfg ? cfg->tap.cooldown_ms : 0, TAP_COOLDOWN_MIN_MS,
                  TAP_COOLDOWN_MAX_MS, TAP_COOLDOWN_DEFAULT_MS);

    session->cfg.battery_green_min =
        clamp_u16(cfg ? cfg->battery_green_min : 0, 1, 100,
                  BATTERY_GREEN_DEFAULT);
    session->cfg.battery_orange_min =
        clamp_u16(cfg ? cfg->battery_orange_min : 0, 1, 100,
                  BATTERY_ORANGE_DEFAULT);
    if (session->cfg.battery_orange_min > session->cfg.battery_green_min) {
        session->cfg.battery_orange_min = session->cfg.battery_green_min;
    }

    mosu_tap_init(&session->tap, &session->cfg.tap);
}

void mosu_session_touch(mosu_session_t *session,
                        const mosu_touch_sample_t *sample)
{
    if (!session || !sample) {
        return;
    }

    if (session->screen_on) {
        session->last_activity_ms = sample->t_ms;
    } else if (sample->pressed) {
        /* Touching a dark screen is a wake gesture, but it must not reset the
         * tap detector: the second half of a double tap may arrive after the
         * panel is already lit. */
        session->wake_pending = true;
    }

    if (mosu_tap_update(&session->tap, sample)) {
        session->say_pending = true;
    }
}

bool mosu_session_take_say(mosu_session_t *session)
{
    if (!session || !session->say_pending) {
        return false;
    }
    session->say_pending = false;
    return true;
}

bool mosu_session_take_wake(mosu_session_t *session)
{
    if (!session || !session->wake_pending) {
        return false;
    }
    session->wake_pending = false;
    return true;
}

void mosu_session_screen_on(mosu_session_t *session, uint32_t now_ms)
{
    if (!session) {
        return;
    }
    session->screen_on = true;
    session->last_activity_ms = now_ms;
    session->wake_pending = false;
}

void mosu_session_screen_off(mosu_session_t *session, uint32_t now_ms)
{
    if (!session) {
        return;
    }
    session->screen_on = false;
    session->last_activity_ms = now_ms;
    session->wake_pending = false;
    /* The tap detector keeps its cooldown and any first tap: a gesture that
     * began in the dark may finish after the panel comes back. */
}

void mosu_session_set_screen_on_ms(mosu_session_t *session, uint32_t ms)
{
    if (!session) {
        return;
    }
    session->cfg.screen_on_ms =
        clamp_u32(ms, MOSU_SESSION_SCREEN_ON_MIN_MS,
                  MOSU_SESSION_SCREEN_ON_MAX_MS,
                  MOSU_SESSION_SCREEN_ON_DEFAULT_MS);
}

bool mosu_session_is_on(const mosu_session_t *session)
{
    return session && session->screen_on;
}

void mosu_session_activity(mosu_session_t *session, uint32_t now_ms)
{
    if (!session) {
        return;
    }
    session->last_activity_ms = now_ms;
}

bool mosu_session_should_sleep(mosu_session_t *session, uint32_t now_ms,
                               bool audio_busy, bool touch_active)
{
    if (!session || !session->screen_on) {
        return false;
    }

    /* Speaking and resting a finger on the glass both keep the face lit. */
    if (audio_busy || touch_active) {
        session->last_activity_ms = now_ms;
        return false;
    }

    return (uint32_t)(now_ms - session->last_activity_ms) >=
           session->cfg.screen_on_ms;
}

uint8_t mosu_session_battery(mosu_session_t *session, uint8_t raw_percent,
                             bool present, bool immediate)
{
    if (!session) {
        return 0;
    }

    if (!present) {
        session->battery_count = 0;
        session->battery_next = 0;
        session->battery_shown = 0;
        session->battery_have = false;
        memset(session->battery_samples, 0, sizeof(session->battery_samples));
        return 0;
    }

    if (raw_percent > 100) {
        raw_percent = 100;
    }

    if (immediate || !session->battery_have) {
        /* A fresh wake is the one moment worth jumping to the truth: the
         * wearer just looked at the watch and should not see a stale value. */
        memset(session->battery_samples, raw_percent,
               sizeof(session->battery_samples));
        session->battery_count = MOSU_SESSION_BATTERY_SAMPLES;
        session->battery_next = 0;
        session->battery_shown = raw_percent;
        session->battery_have = true;
        return session->battery_shown;
    }

    session->battery_samples[session->battery_next] = raw_percent;
    session->battery_next =
        (uint8_t)((session->battery_next + 1) % MOSU_SESSION_BATTERY_SAMPLES);
    if (session->battery_count < MOSU_SESSION_BATTERY_SAMPLES) {
        session->battery_count++;
    }

    unsigned sum = 0;
    for (uint8_t i = 0; i < session->battery_count; i++) {
        sum += session->battery_samples[i];
    }
    session->battery_shown =
        (uint8_t)((sum + session->battery_count / 2u) /
                  session->battery_count);
    return session->battery_shown;
}

mosu_batt_bucket_t mosu_session_battery_bucket(const mosu_session_t *session)
{
    if (!session || !session->battery_have) {
        return MOSU_BATT_UNKNOWN;
    }
    return mosu_battery_bucket(session->battery_shown,
                               (uint8_t)session->cfg.battery_green_min,
                               (uint8_t)session->cfg.battery_orange_min);
}

mosu_session_wifi_t
mosu_session_wifi_icon(mosu_session_net_state_t net_state, bool have_sync)
{
    if (net_state == MOSU_SESSION_NET_FAILED) {
        return MOSU_SESSION_WIFI_FAIL;
    }
    if (have_sync) {
        return MOSU_SESSION_WIFI_OK;
    }
    return MOSU_SESSION_WIFI_IDLE;
}

mosu_session_wifi_t mosu_session_wifi_from_age(uint32_t age_s,
                                               uint32_t interval_s)
{
    if (age_s == UINT32_MAX) {
        return MOSU_SESSION_WIFI_IDLE;
    }
    if (interval_s < 3600u) {
        interval_s = 3600u;
    }
    if (interval_s > 168u * 3600u) {
        interval_s = 168u * 3600u;
    }
    /* One and a half intervals: a single missed attempt, or a slow retry,
     * keeps the icon green because the clock is still good. */
    if (age_s <= interval_s + interval_s / 2u) {
        return MOSU_SESSION_WIFI_OK;
    }
    return MOSU_SESSION_WIFI_FAIL;
}
