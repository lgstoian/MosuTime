/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Host tests for the pure session state machine: screen timing, touch and
 * double tap behaviour across a screen transition, battery smoothing and the
 * Wi-Fi icon rules.
 */
#include <string.h>

#include "mosu_session.h"
#include "test_util.h"

static const mosu_session_cfg_t s_cfg = {
    .screen_on_ms = 15000,
    .tap =
        {
            .double_tap_ms = 650,
            .max_hold_ms = 400,
            .max_move_px = 40,
            .cooldown_ms = 2000,
        },
    .battery_green_min = 40,
    .battery_orange_min = 15,
};

/* A complete double tap: press/release, a short gap, press/release. */
static void feed_double_tap(mosu_session_t *session, uint32_t first_release,
                            uint32_t second_press)
{
    mosu_touch_sample_t sample = {true, 200, 250, first_release - 100};
    mosu_session_touch(session, &sample);

    sample = (mosu_touch_sample_t){false, 200, 250, first_release};
    mosu_session_touch(session, &sample);

    sample = (mosu_touch_sample_t){true, 200, 250, second_press};
    mosu_session_touch(session, &sample);

    sample = (mosu_touch_sample_t){false, 200, 250, second_press + 100};
    mosu_session_touch(session, &sample);
}

static void test_screen_timing(void)
{
    mosu_session_t session;
    mosu_session_init(&session, &s_cfg);
    EXPECT(!mosu_session_is_on(&session));

    /* Already asleep: there is nothing to put to sleep. */
    EXPECT(!mosu_session_should_sleep(&session, 1000, false, false));

    mosu_session_screen_on(&session, 1000);
    EXPECT(mosu_session_is_on(&session));
    EXPECT(!mosu_session_should_sleep(&session, 1000 + 14999, false, false));
    EXPECT(mosu_session_should_sleep(&session, 1000 + 15000, false, false));

    mosu_session_screen_off(&session, 17000);
    EXPECT(!mosu_session_is_on(&session));
}

static void test_touch_extends(void)
{
    mosu_session_t session;
    mosu_session_init(&session, &s_cfg);
    mosu_session_screen_on(&session, 1000);

    mosu_touch_sample_t press = {true, 100, 100, 5000};
    mosu_session_touch(&session, &press);

    EXPECT(!mosu_session_should_sleep(&session, 5000 + 14999, false, false));
    EXPECT(mosu_session_should_sleep(&session, 5000 + 15000, false, false));
}

static void test_audio_busy_extends(void)
{
    mosu_session_t session;
    mosu_session_init(&session, &s_cfg);
    mosu_session_screen_on(&session, 1000);

    /* The nominal deadline passes while the watch is speaking. */
    EXPECT(!mosu_session_should_sleep(&session, 16000, true, false));

    /* The deadline moved to 16000, so the screen is still lit one tick
     * later, and only goes dark after another full screen_on_ms. */
    EXPECT(!mosu_session_should_sleep(&session, 16001, false, false));
    EXPECT(!mosu_session_should_sleep(&session, 16000 + 14999, false, false));
    EXPECT(mosu_session_should_sleep(&session, 16000 + 15000, false, false));
}

static void test_touch_during_speech(void)
{
    mosu_session_t session;
    mosu_session_init(&session, &s_cfg);
    mosu_session_screen_on(&session, 1000);

    /* Audio busy at 5000 and a resting finger at 6000 both refresh. */
    EXPECT(!mosu_session_should_sleep(&session, 5000, true, false));
    EXPECT(!mosu_session_should_sleep(&session, 6000, false, true));
    EXPECT(!mosu_session_should_sleep(&session, 7000, false, false));
    EXPECT(mosu_session_should_sleep(&session, 6000 + 15000, false, false));
}

static void test_double_tap(void)
{
    mosu_session_t session;
    mosu_session_init(&session, &s_cfg);
    mosu_session_screen_on(&session, 0);

    feed_double_tap(&session, 1000, 1400);
    EXPECT(mosu_session_take_say(&session));
    EXPECT(!mosu_session_take_say(&session));

    /* The cooldown is still running, also across a screen transition. */
    feed_double_tap(&session, 2000, 2100);
    EXPECT(!mosu_session_take_say(&session));

    mosu_session_screen_off(&session, 2200);
    feed_double_tap(&session, 3000, 3100);
    EXPECT(!mosu_session_take_say(&session));

    /* After the cooldown the same gesture works again. */
    feed_double_tap(&session, 4000, 4200);
    EXPECT(mosu_session_take_say(&session));
}

static void test_double_tap_from_dark(void)
{
    mosu_session_t session;
    mosu_session_init(&session, &s_cfg);

    /* First half of the gesture happens with the panel off. */
    mosu_touch_sample_t sample = {true, 200, 250, 1000};
    mosu_session_touch(&session, &sample);
    EXPECT(mosu_session_take_wake(&session));
    EXPECT(!mosu_session_take_wake(&session));

    sample = (mosu_touch_sample_t){false, 200, 250, 1100};
    mosu_session_touch(&session, &sample);

    /* The application wakes the panel; the detector must not be reset. */
    mosu_session_screen_on(&session, 1100);
    EXPECT(!mosu_session_take_wake(&session));

    sample = (mosu_touch_sample_t){true, 200, 250, 1200};
    mosu_session_touch(&session, &sample);
    sample = (mosu_touch_sample_t){false, 200, 250, 1300};
    mosu_session_touch(&session, &sample);

    EXPECT(mosu_session_take_say(&session));

    /* A gesture that is entirely in the dark is still recognised. */
    mosu_session_t dark;
    mosu_session_init(&dark, &s_cfg);
    feed_double_tap(&dark, 1000, 1400);
    EXPECT(mosu_session_take_say(&dark));
    EXPECT(mosu_session_take_wake(&dark));
}

static void test_config_clamp(void)
{
    mosu_session_t session;

    mosu_session_init(&session, NULL);
    EXPECT(session.cfg.screen_on_ms == MOSU_SESSION_SCREEN_ON_DEFAULT_MS);
    EXPECT(session.cfg.tap.double_tap_ms == 650);
    EXPECT(session.cfg.tap.max_hold_ms == 400);
    EXPECT(session.cfg.tap.max_move_px == 40);
    EXPECT(session.cfg.tap.cooldown_ms == 2000);
    EXPECT(session.cfg.battery_green_min == 40);
    EXPECT(session.cfg.battery_orange_min == 15);

    /* Every zero field falls back to something usable. */
    mosu_session_cfg_t zero;
    memset(&zero, 0, sizeof(zero));
    mosu_session_init(&session, &zero);
    EXPECT(session.cfg.screen_on_ms == MOSU_SESSION_SCREEN_ON_DEFAULT_MS);
    EXPECT(session.cfg.tap.double_tap_ms == 650);
    EXPECT(session.cfg.tap.max_hold_ms == 400);
    EXPECT(session.cfg.tap.max_move_px == 40);
    EXPECT(session.cfg.tap.cooldown_ms == 2000);
    EXPECT(session.cfg.battery_green_min == 40);
    EXPECT(session.cfg.battery_orange_min == 15);

    /* Out of range values are clamped, and orange never exceeds green. */
    mosu_session_cfg_t wild = {
        .screen_on_ms = 999999,
        .tap = {.double_tap_ms = 1,
                .max_hold_ms = 1,
                .max_move_px = 1,
                .cooldown_ms = 1},
        .battery_green_min = 200,
        .battery_orange_min = 300,
    };
    mosu_session_init(&session, &wild);
    EXPECT(session.cfg.screen_on_ms == MOSU_SESSION_SCREEN_ON_MAX_MS);
    EXPECT(session.cfg.tap.double_tap_ms == 200);
    EXPECT(session.cfg.tap.max_hold_ms == 80);
    EXPECT(session.cfg.tap.max_move_px == 4);
    EXPECT(session.cfg.tap.cooldown_ms == 300);
    EXPECT(session.cfg.battery_green_min == 100);
    EXPECT(session.cfg.battery_orange_min == 100);

    mosu_session_cfg_t swapped = s_cfg;
    swapped.battery_green_min = 30;
    swapped.battery_orange_min = 50;
    mosu_session_init(&session, &swapped);
    EXPECT(session.cfg.battery_orange_min == 30);
}

static void test_battery_smoothing(void)
{
    mosu_session_t session;
    mosu_session_init(&session, &s_cfg);

    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_UNKNOWN);

    /* The first sample is shown as measured. */
    EXPECT(mosu_session_battery(&session, 20, true, false) == 20);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_ORANGE);

    /* Rising towards green, one quarter of the window at a time. */
    uint8_t shown = mosu_session_battery(&session, 60, true, false);
    EXPECT(shown == 30);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_ORANGE);
    shown = mosu_session_battery(&session, 60, true, false);
    EXPECT(shown == 40);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_GREEN);
    shown = mosu_session_battery(&session, 60, true, false);
    EXPECT(shown == 50);
    shown = mosu_session_battery(&session, 60, true, false);
    EXPECT(shown == 60);

    /* Falling towards red, monotonically. */
    mosu_session_init(&session, &s_cfg);
    EXPECT(mosu_session_battery(&session, 80, true, false) == 80);
    uint8_t previous = 80;
    shown = mosu_session_battery(&session, 40, true, false);
    EXPECT(shown == 70);
    EXPECT(shown < previous);
    previous = shown;
    shown = mosu_session_battery(&session, 40, true, false);
    EXPECT(shown == 60);
    EXPECT(shown < previous);
    previous = shown;
    shown = mosu_session_battery(&session, 40, true, false);
    EXPECT(shown == 50);
    EXPECT(shown < previous);
    previous = shown;
    shown = mosu_session_battery(&session, 40, true, false);
    EXPECT(shown == 40);
    EXPECT(shown < previous);

    /* An immediate reading snaps, and a removed pack clears the history. */
    EXPECT(mosu_session_battery(&session, 90, true, true) == 90);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_GREEN);
    EXPECT(mosu_session_battery(&session, 0, false, false) == 0);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_UNKNOWN);
}

static void test_battery_bucket_edges(void)
{
    mosu_session_t session;
    mosu_session_init(&session, &s_cfg);

    EXPECT(mosu_session_battery(&session, 40, true, true) == 40);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_GREEN);
    EXPECT(mosu_session_battery(&session, 39, true, true) == 39);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_ORANGE);
    EXPECT(mosu_session_battery(&session, 15, true, true) == 15);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_ORANGE);
    EXPECT(mosu_session_battery(&session, 14, true, true) == 14);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_RED);
    EXPECT(mosu_session_battery(&session, 100, true, true) == 100);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_GREEN);
    EXPECT(mosu_session_battery(&session, 0, true, true) == 0);
    EXPECT(mosu_session_battery_bucket(&session) == MOSU_BATT_RED);
}

static void test_wifi_icon(void)
{
    /* A failed link wins even when an earlier sync succeeded. */
    EXPECT(mosu_session_wifi_icon(MOSU_SESSION_NET_FAILED, true) ==
           MOSU_SESSION_WIFI_FAIL);
    EXPECT(mosu_session_wifi_icon(MOSU_SESSION_NET_FAILED, false) ==
           MOSU_SESSION_WIFI_FAIL);

    EXPECT(mosu_session_wifi_icon(MOSU_SESSION_NET_CONNECTED, true) ==
           MOSU_SESSION_WIFI_OK);
    EXPECT(mosu_session_wifi_icon(MOSU_SESSION_NET_CONNECTING, true) ==
           MOSU_SESSION_WIFI_OK);
    EXPECT(mosu_session_wifi_icon(MOSU_SESSION_NET_CONNECTED, false) ==
           MOSU_SESSION_WIFI_IDLE);
    EXPECT(mosu_session_wifi_icon(MOSU_SESSION_NET_IDLE, false) ==
           MOSU_SESSION_WIFI_IDLE);
}

void test_session(void)
{
    test_screen_timing();
    test_touch_extends();
    test_audio_busy_extends();
    test_touch_during_speech();
    test_double_tap();
    test_double_tap_from_dark();
    test_config_clamp();
    test_battery_smoothing();
    test_battery_bucket_edges();
    test_wifi_icon();
}
