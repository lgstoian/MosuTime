/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Host tests for the pure network policy: numeric UTC offsets, the POSIX
 * spelling, geolocation replies and the post-failure sync schedule.
 */
#include <stdio.h>
#include <string.h>

#include "mosu_policy.h"
#include "test_util.h"

static void test_offsets(void)
{
    char out[MOSU_POLICY_TZ_MAX];

    /* Seconds, as returned by ip-api. */
    EXPECT(mosu_policy_offset_to_posix("7200", out, sizeof(out)));
    EXPECT_STR(out, "UTC-2");
    EXPECT(mosu_policy_offset_to_posix("+7200", out, sizeof(out)));
    EXPECT_STR(out, "UTC-2");
    EXPECT(mosu_policy_offset_to_posix("-18000", out, sizeof(out)));
    EXPECT_STR(out, "UTC+5");
    EXPECT(mosu_policy_offset_to_posix("19800", out, sizeof(out)));
    EXPECT_STR(out, "UTC-5:30");
    EXPECT(mosu_policy_offset_to_posix("-12600", out, sizeof(out)));
    EXPECT_STR(out, "UTC+3:30");
    EXPECT(mosu_policy_offset_to_posix("3600", out, sizeof(out)));
    EXPECT_STR(out, "UTC-1");
    EXPECT(mosu_policy_offset_to_posix("3661", out, sizeof(out)));
    EXPECT_STR(out, "UTC-1:01:01");

    /* Bare hours, including a half hour and both signs. */
    EXPECT(mosu_policy_offset_to_posix("2", out, sizeof(out)));
    EXPECT_STR(out, "UTC-2");
    EXPECT(mosu_policy_offset_to_posix("+2", out, sizeof(out)));
    EXPECT_STR(out, "UTC-2");
    EXPECT(mosu_policy_offset_to_posix("-5", out, sizeof(out)));
    EXPECT_STR(out, "UTC+5");
    EXPECT(mosu_policy_offset_to_posix("-5.5", out, sizeof(out)));
    EXPECT_STR(out, "UTC+5:30");
    EXPECT(mosu_policy_offset_to_posix("5.5", out, sizeof(out)));
    EXPECT_STR(out, "UTC-5:30");
    EXPECT(mosu_policy_offset_to_posix("-0.5", out, sizeof(out)));
    EXPECT_STR(out, "UTC+0:30");

    /* The edges of the accepted range. */
    EXPECT(mosu_policy_offset_to_posix("14", out, sizeof(out)));
    EXPECT_STR(out, "UTC-14");
    EXPECT(mosu_policy_offset_to_posix("-12", out, sizeof(out)));
    EXPECT_STR(out, "UTC+12");

    /* Zero is spelled the way the built-in table spells it. */
    EXPECT(mosu_policy_offset_to_posix("0", out, sizeof(out)));
    EXPECT_STR(out, "UTC0");
    EXPECT(mosu_policy_offset_to_posix("+0", out, sizeof(out)));
    EXPECT_STR(out, "UTC0");

    /* Syntax only: the helper must not mistake a TZ string for a number. */
    EXPECT(mosu_policy_is_offset("7200"));
    EXPECT(mosu_policy_is_offset("-5.5"));
    EXPECT(!mosu_policy_is_offset("UTC0"));
    EXPECT(!mosu_policy_is_offset("Europe/Bucharest"));
}

static void test_offsets_invalid(void)
{
    char out[MOSU_POLICY_TZ_MAX];
    char small[4];

    EXPECT(!mosu_policy_offset_to_posix(NULL, out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("abc", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("1e3", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("12x", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("2..5", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("2.", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("+", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("-", out, sizeof(out)));

    /* 19..59 is ambiguous: too big for hours, too small for seconds. */
    EXPECT(!mosu_policy_offset_to_posix("19", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("59", out, sizeof(out)));

    /* Out of range, or too large to be anything real. */
    EXPECT(!mosu_policy_offset_to_posix("15", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("-13", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("54000", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("999999", out, sizeof(out)));
    EXPECT(!mosu_policy_offset_to_posix("36000.5", out, sizeof(out)));

    /* A too-small output buffer is a clean failure, not a truncation. */
    EXPECT(!mosu_policy_offset_to_posix("7200", small, sizeof(small)));
    EXPECT(!mosu_policy_offset_to_posix("7200", out, 0));
}

static void test_geo(void)
{
    mosu_policy_geo_t geo;

    /* The new default: ip-api's two-line name + offset. */
    mosu_policy_parse_geo("Europe/Bucharest\n7200\n", &geo);
    EXPECT_STR(geo.zone, "Europe/Bucharest");
    EXPECT_STR(geo.posix, "UTC-2");

    /* Old JSON endpoint, with whitespace and a newline. */
    mosu_policy_parse_geo(
        "{\n  \"timezone\": \"Europe/Bucharest\",\n  \"offset\": 10800\n}",
        &geo);
    EXPECT_STR(geo.zone, "Europe/Bucharest");
    EXPECT_STR(geo.posix, "UTC-3");

    /* Plain text from ipapi.co/timezone/. */
    mosu_policy_parse_geo("Europe/Bucharest\n", &geo);
    EXPECT_STR(geo.zone, "Europe/Bucharest");
    EXPECT_STR(geo.posix, "");

    mosu_policy_parse_geo("Europe/Bucharest", &geo);
    EXPECT_STR(geo.zone, "Europe/Bucharest");
    EXPECT_STR(geo.posix, "");

    /* Offset only is still useful when the name is missing. */
    mosu_policy_parse_geo("7200\n", &geo);
    EXPECT_STR(geo.zone, "");
    EXPECT_STR(geo.posix, "UTC-2");

    /* An error body must leave both fields empty. */
    mosu_policy_parse_geo("fail\n", &geo);
    EXPECT_STR(geo.zone, "");
    EXPECT_STR(geo.posix, "");

    mosu_policy_parse_geo("<html><body>oops</body></html>", &geo);
    EXPECT_STR(geo.zone, "");
    EXPECT_STR(geo.posix, "");

    mosu_policy_parse_geo("", &geo);
    EXPECT_STR(geo.zone, "");
    EXPECT_STR(geo.posix, "");

    mosu_policy_parse_geo(NULL, &geo);
    EXPECT_STR(geo.zone, "");
    EXPECT_STR(geo.posix, "");

    /* "fail" is not a zone, but a numeric offset next to it still helps. */
    mosu_policy_parse_geo("{\"timezone\":\"fail\",\"offset\":7200}", &geo);
    EXPECT_STR(geo.zone, "");
    EXPECT_STR(geo.posix, "UTC-2");
}

static void test_zone_plausible(void)
{
    EXPECT(mosu_policy_zone_plausible("Europe/Bucharest"));
    EXPECT(mosu_policy_zone_plausible("America/Argentina/Buenos_Aires"));
    EXPECT(mosu_policy_zone_plausible("Etc/UTC"));
    EXPECT(mosu_policy_zone_plausible("UTC"));
    EXPECT(mosu_policy_zone_plausible("UTC0"));
    EXPECT(mosu_policy_zone_plausible("EET-2EEST,M3.5.0/3,M10.5.0/4"));

    EXPECT(!mosu_policy_zone_plausible(NULL));
    EXPECT(!mosu_policy_zone_plausible(""));
    EXPECT(!mosu_policy_zone_plausible("fail"));
    EXPECT(!mosu_policy_zone_plausible("zzzz"));
    EXPECT(!mosu_policy_zone_plausible("<html>"));
    EXPECT(!mosu_policy_zone_plausible("Europe//Bucharest"));
    EXPECT(!mosu_policy_zone_plausible("/Bucharest"));
    EXPECT(!mosu_policy_zone_plausible("Europe/"));
    EXPECT(!mosu_policy_zone_plausible("Europe/Bucharest;rm"));
}

static void test_backoff(void)
{
    const uint32_t hour = 3600;

    /* Success resets to the normal interval. */
    EXPECT(mosu_policy_next_sync_s(0, 6 * hour) == 6 * hour);
    EXPECT(mosu_policy_next_sync_s(0, 1 * hour) == hour);

    /* Five syncs a day is 4.8 hours, not a whole number of hours. */
    EXPECT(mosu_policy_next_sync_s(0, 86400 / 5) == 17280);

    /* Three bounded retries: 5, 15 and 30 minutes. */
    EXPECT(mosu_policy_next_sync_s(1, 6 * hour) == 5 * 60);
    EXPECT(mosu_policy_next_sync_s(2, 6 * hour) == 15 * 60);
    EXPECT(mosu_policy_next_sync_s(3, 6 * hour) == 30 * 60);

    /* After the third retry, wait the configured interval again. */
    EXPECT(mosu_policy_next_sync_s(4, 6 * hour) == 6 * hour);
    EXPECT(mosu_policy_next_sync_s(5, 6 * hour) == 6 * hour);
    EXPECT(mosu_policy_next_sync_s(255, 6 * hour) == 6 * hour);

    /* The retries never exceed a one-hour interval. */
    EXPECT(mosu_policy_next_sync_s(3, 1 * hour) == 30 * 60);
    EXPECT(mosu_policy_next_sync_s(3, 1 * hour) < hour);

    /* A zero interval is clamped instead of causing a busy loop. */
    EXPECT(mosu_policy_next_sync_s(0, 0) == hour);
    EXPECT(mosu_policy_next_sync_s(4, 0) == hour);
}

static void test_wifi_learn(void)
{
    mosu_wifi_learn_t learn;
    mosu_wifi_learn_init(&learn);

    EXPECT(mosu_wifi_learn_score(&learn, 9) == 128);
    EXPECT(mosu_wifi_learn_scan_s(&learn, 9) == 60 * 60);

    mosu_wifi_learn_note(&learn, 9, true);
    EXPECT(mosu_wifi_learn_score(&learn, 9) > 128);
    for (int i = 0; i < 20; i++) {
        mosu_wifi_learn_note(&learn, 9, true);
    }
    EXPECT(mosu_wifi_learn_scan_s(&learn, 9) == 15 * 60);

    for (int i = 0; i < 200; i++) {
        mosu_wifi_learn_note(&learn, 3, false);
    }
    EXPECT(mosu_wifi_learn_score(&learn, 3) < 24);
    EXPECT(mosu_wifi_learn_scan_s(&learn, 3) == 6 * 60 * 60);

    /* An invalid hour is ignored, not written out of bounds. */
    mosu_wifi_learn_note(&learn, 24, true);
    EXPECT(mosu_wifi_learn_score(&learn, 24) == 128);
    EXPECT(mosu_wifi_learn_scan_s(&learn, 24) == 60 * 60);
}

static void test_power_learn(void)
{
    mosu_power_learn_t learn;
    mosu_power_learn_init(&learn);
    EXPECT(mosu_power_learn_angle(&learn, 50) == 50);
    EXPECT(mosu_power_learn_screen_ms(&learn, 15000) == 15000);

    /* A busy hour cannot change the policy on its own: the first tick starts
     * the cooldown. */
    for (int i = 0; i < 60; i++) {
        mosu_power_learn_note(&learn, 60000, true);
    }
    EXPECT(!mosu_power_learn_tick(&learn, 3600000));

    /* Three busy hours take one step. */
    for (int h = 0; h < 3; h++) {
        for (int i = 0; i < 60; i++) {
            mosu_power_learn_note(&learn, 60000, true);
        }
        mosu_power_learn_tick(&learn, 3600000);
    }
    EXPECT(learn.angle_bonus > 0);
    EXPECT(mosu_power_learn_angle(&learn, 50) > 50);
    EXPECT(mosu_power_learn_screen_ms(&learn, 15000) < 15000);

    /* The offsets stop at their bounds. */
    for (int h = 0; h < 60; h++) {
        for (int i = 0; i < 60; i++) {
            mosu_power_learn_note(&learn, 60000, true);
        }
        mosu_power_learn_tick(&learn, 3600000);
    }
    EXPECT(learn.angle_bonus == MOSU_POWER_ANGLE_MAX);
    EXPECT(learn.screen_cut_ms == MOSU_POWER_CUT_MAX_MS);
    EXPECT(mosu_power_learn_angle(&learn, 70) == 80);
    EXPECT(mosu_power_learn_screen_ms(&learn, 9000) ==
           MOSU_POWER_SCREEN_MIN_MS);

    /* Calm hours give a step back, never below the configured values. */
    for (int h = 0; h < 12; h++) {
        for (int i = 0; i < 60; i++) {
            mosu_power_learn_note(&learn, 60000, false);
        }
        mosu_power_learn_tick(&learn, 3600000);
    }
    EXPECT(learn.angle_bonus < MOSU_POWER_ANGLE_MAX);
}

void test_policy(void)
{
    test_offsets();
    test_offsets_invalid();
    test_geo();
    test_zone_plausible();
    test_backoff();
    test_wifi_learn();
    test_power_learn();
}
