/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Host-side tests for the pure C core and the INI parser.
 * Build:  cmake -S test/host -B test/host/build && cmake --build test/host/build
 * Run:    ./test/host/build/mosu_tests
 */
#include <stdio.h>
#include <string.h>

#include "mosu_config.h"
#include "mosu_core/mosu_core.h"
#include "test_util.h"

int g_failures;
int g_checks;

static void test_12h(void)
{
    uint8_t h = 0;
    bool pm = false;

    mosu_time_split12(0, &h, &pm);
    EXPECT(h == 12 && !pm);
    mosu_time_split12(11, &h, &pm);
    EXPECT(h == 11 && !pm);
    mosu_time_split12(12, &h, &pm);
    EXPECT(h == 12 && pm);
    mosu_time_split12(13, &h, &pm);
    EXPECT(h == 1 && pm);
    mosu_time_split12(23, &h, &pm);
    EXPECT(h == 11 && pm);

    char hours[3];
    char minutes[3];
    mosu_format_clock(13, 5, true, hours, minutes);
    EXPECT_STR(hours, "1");
    EXPECT_STR(minutes, "05");
    mosu_format_clock(0, 0, true, hours, minutes);
    EXPECT_STR(hours, "12");
    EXPECT_STR(minutes, "00");
    mosu_format_clock(0, 0, false, hours, minutes);
    EXPECT_STR(hours, "--");
    EXPECT_STR(minutes, "--");
}

static void test_phrase(void)
{
    mosu_phrase_t p;

    mosu_phrase_build(14, 30, 1, true, &p);
    EXPECT(p.valid);
    EXPECT(p.hour12 == 2 && p.pm);
    EXPECT(p.count == 3);
    EXPECT(p.clips[0].kind == MOSU_CLIP_HOUR && p.clips[0].index == 2);
    EXPECT(p.clips[1].kind == MOSU_CLIP_MINUTE && p.clips[1].index == 30);
    EXPECT(p.clips[2].kind == MOSU_CLIP_DAY && p.clips[2].index == 1);

    mosu_phrase_build(9, 0, 0, true, &p);
    EXPECT(p.count == 2);
    EXPECT(p.clips[0].index == 9);
    EXPECT(p.clips[1].kind == MOSU_CLIP_DAY);

    mosu_phrase_build(9, 0, 0, false, &p);
    EXPECT(p.count == 1);

    mosu_phrase_build(0, 1, 6, false, &p);
    EXPECT(p.hour12 == 12 && !p.pm);
    EXPECT(p.clips[1].kind == MOSU_CLIP_MINUTE && p.clips[1].index == 1);

    EXPECT(mosu_weekday_from_tm(0) == 6);
    EXPECT(mosu_weekday_from_tm(1) == 0);
    EXPECT(mosu_weekday_from_tm(6) == 5);

    char name[16];
    EXPECT(mosu_clip_filename(MOSU_CLIP_HOUR, 3, name, sizeof(name)));
    EXPECT_STR(name, "h_3.wav");
    EXPECT(mosu_clip_filename(MOSU_CLIP_MINUTE, 45, name, sizeof(name)));
    EXPECT_STR(name, "m_45.wav");
    EXPECT(mosu_clip_filename(MOSU_CLIP_DAY, 2, name, sizeof(name)));
    EXPECT_STR(name, "d_2.wav");
    EXPECT(!mosu_clip_filename(MOSU_CLIP_HOUR, 13, name, sizeof(name)));
    EXPECT(!mosu_clip_filename(MOSU_CLIP_MINUTE, 0, name, sizeof(name)));
}

static void test_battery(void)
{
    EXPECT(mosu_battery_percent(4250) == 100);
    EXPECT(mosu_battery_percent(4200) == 100);
    EXPECT(mosu_battery_percent(3500) < 10);
    EXPECT(mosu_battery_percent(3300) == 0);
    EXPECT(mosu_battery_percent(1000) == 0);

    uint8_t previous = 0;
    for (uint16_t mv = 3300; mv <= 4200; mv += 10) {
        uint8_t percent = mosu_battery_percent(mv);
        EXPECT(percent >= previous);
        previous = percent;
    }

    EXPECT(mosu_battery_bucket(80, 40, 15) == MOSU_BATT_GREEN);
    EXPECT(mosu_battery_bucket(40, 40, 15) == MOSU_BATT_GREEN);
    EXPECT(mosu_battery_bucket(39, 40, 15) == MOSU_BATT_ORANGE);
    EXPECT(mosu_battery_bucket(15, 40, 15) == MOSU_BATT_ORANGE);
    EXPECT(mosu_battery_bucket(14, 40, 15) == MOSU_BATT_RED);
}

static bool tap_sequence(mosu_tap_t *tap, uint32_t first_release,
                         uint32_t second_press)
{
    mosu_touch_sample_t sample;
    bool fired = false;

    sample = (mosu_touch_sample_t){true, 200, 250, first_release - 100};
    mosu_tap_update(tap, &sample);
    sample = (mosu_touch_sample_t){false, 200, 250, first_release};
    EXPECT(!mosu_tap_update(tap, &sample));

    sample = (mosu_touch_sample_t){true, 200, 250, second_press};
    mosu_tap_update(tap, &sample);
    sample = (mosu_touch_sample_t){false, 200, 250, second_press + 100};
    fired = mosu_tap_update(tap, &sample);
    return fired;
}

static void test_tap(void)
{
    mosu_tap_cfg_t cfg = {
        .double_tap_ms = 650,
        .max_hold_ms = 400,
        .max_move_px = 40,
        .cooldown_ms = 2000,
    };
    mosu_tap_t tap;
    mosu_tap_init(&tap, &cfg);

    EXPECT(tap_sequence(&tap, 1000, 1400));

    /* Cooldown blocks the next gesture. */
    EXPECT(!tap_sequence(&tap, 2000, 2100));

    mosu_tap_init(&tap, &cfg);
    /* Too slow. */
    EXPECT(!tap_sequence(&tap, 1000, 1800));

    mosu_tap_init(&tap, &cfg);
    /* Finger moved too far. */
    mosu_touch_sample_t s = {true, 100, 100, 1000};
    mosu_tap_update(&tap, &s);
    s = (mosu_touch_sample_t){false, 250, 100, 1080};
    mosu_tap_update(&tap, &s);
    s = (mosu_touch_sample_t){true, 100, 100, 1200};
    mosu_tap_update(&tap, &s);
    s = (mosu_touch_sample_t){false, 100, 100, 1280};
    EXPECT(!mosu_tap_update(&tap, &s));

    mosu_tap_init(&tap, &cfg);
    /* Long press is not a tap. */
    s = (mosu_touch_sample_t){true, 100, 100, 1000};
    mosu_tap_update(&tap, &s);
    s = (mosu_touch_sample_t){false, 100, 100, 1600};
    mosu_tap_update(&tap, &s);
    s = (mosu_touch_sample_t){true, 100, 100, 1700};
    mosu_tap_update(&tap, &s);
    s = (mosu_touch_sample_t){false, 100, 100, 1750};
    EXPECT(!mosu_tap_update(&tap, &s));
}

static void test_wrist(void)
{
    mosu_wrist_cfg_t cfg = {.angle_mdeg = 35000, .refractory_ms = 2500};
    mosu_wrist_t wrist;
    mosu_wrist_init(&wrist);

    mosu_accel_t down = {0.0f, 0.0f, 1.0f};
    mosu_accel_t up = {1.0f, 0.0f, 0.0f};

    EXPECT(!mosu_wrist_update(&wrist, &down, &cfg, 1000));
    EXPECT(!mosu_wrist_update(&wrist, &up, &cfg, 1100));
    EXPECT(mosu_wrist_update(&wrist, &up, &cfg, 1300));

    /* Holding the new orientation does not retrigger. */
    EXPECT(!mosu_wrist_update(&wrist, &up, &cfg, 2000));
    EXPECT(!mosu_wrist_update(&wrist, &up, &cfg, 5000));

    /* Small wobble around the baseline is ignored. */
    mosu_wrist_init(&wrist);
    EXPECT(!mosu_wrist_update(&wrist, &down, &cfg, 1000));
    mosu_accel_t wobble = {0.1f, 0.0f, 0.99f};
    EXPECT(!mosu_wrist_update(&wrist, &wobble, &cfg, 1100));
    EXPECT(!mosu_wrist_update(&wrist, &wobble, &cfg, 1300));

    /* Lowering the arm is also a large change: it must respect refractory. */
    EXPECT(!mosu_wrist_update(&wrist, &up, &cfg, 2000));
    EXPECT(mosu_wrist_update(&wrist, &up, &cfg, 2200));
    EXPECT(!mosu_wrist_update(&wrist, &down, &cfg, 2400));
}

static void test_config(void)
{
    mosu_config_t cfg;
    mosu_config_report_t report;
    mosu_config_defaults(&cfg);

    const char *ini =
        "# MosuTime test\n"
        "[device]\n"
        "name = \"MosuTime Test\"   # trailing note\n"
        "log_level = debug\n"
        "\n"
        "[display]\n"
        "brightness = 42\n"
        "screen_on_ms = 9000\n"
        "wake_on_motion = no\n"
        "pixel_shift_sec = 0\n"
        "\n"
        "[colors]\n"
        "background = #000000\n"
        "time_am = #FFFFFF\n"
        "time_pm = #F80\n"
        "\n"
        "[status]\n"
        "battery_green_min = 50\n"
        "battery_orange_min = 20\n"
        "\n"
        "[clock]\n"
        "timezone = Europe/Bucharest\n"
        "auto_timezone = true\n"
        "sync_hours = 12\n"
        "\n"
        "[wifi]\n"
        "network2_ssid = Bunica\n"
        "network2_pass = secret\n"
        "network1_ssid = Acasa\n"
        "network1_pass = \"parola\"\n"
        "\n"
        "[announce]\n"
        "volume = 70\n"
        "voice_dir = /sdcard/voice\n";

    int rc = mosu_config_parse(&cfg, ini, strlen(ini), &report);
    EXPECT(rc == 0);
    EXPECT(report.errors == 0);
    EXPECT_STR(cfg.device.name, "MosuTime Test");
    EXPECT(cfg.device.log_level == 4);
    EXPECT(cfg.display.brightness == 42);
    EXPECT(cfg.display.screen_on_ms == 9000);
    EXPECT(!cfg.display.wake_on_motion);
    EXPECT(cfg.display.pixel_shift_sec == 0);
    EXPECT(cfg.colors.background == 0x000000);
    EXPECT(cfg.colors.time_am == 0xFFFFFF);
    EXPECT(cfg.colors.time_pm == 0xFF8800);
    EXPECT(cfg.status.battery_green_min == 50);
    EXPECT(cfg.status.battery_orange_min == 20);
    EXPECT_STR(cfg.clock.timezone, "Europe/Bucharest");
    EXPECT(cfg.clock.sync_hours == 12);
    EXPECT(cfg.wifi_count == 2);
    EXPECT_STR(cfg.wifi[0].ssid, "Acasa");
    EXPECT_STR(cfg.wifi[0].pass, "parola");
    EXPECT_STR(cfg.wifi[1].ssid, "Bunica");
    EXPECT(cfg.announce.volume == 70);

    /* Invalid values must be reported, and the old value kept. */
    mosu_config_defaults(&cfg);
    const char *bad =
        "[display]\n"
        "brightness = banana\n"
        "wake_on_touch = perhaps\n"
        "[colors]\n"
        "time_am = #12\n"
        "[clock]\n"
        "timezone = $(rm -rf /)\n";
    rc = mosu_config_parse(&cfg, bad, strlen(bad), &report);
    EXPECT(rc == 0);
    EXPECT(report.errors == 4);
    EXPECT(cfg.display.brightness == 55);
    EXPECT(cfg.display.wake_on_touch);
    EXPECT(cfg.colors.time_am == 0xFFFFFF);
    EXPECT_STR(cfg.clock.timezone, "Europe/Bucharest");

    EXPECT(mosu_config_timezone_ok("EET-2EEST,M3.5.0/3,M10.5.0/4"));
    EXPECT(mosu_config_timezone_ok("Europe/Bucharest"));
    EXPECT(!mosu_config_timezone_ok(""));
    EXPECT(!mosu_config_timezone_ok("bad;rm"));
}

int main(void)
{
    test_12h();
    test_phrase();
    test_battery();
    test_tap();
    test_wrist();
    test_config();
    test_session();
    test_config_extra();
    test_bmp();
    test_policy();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("no checks were executed\n");
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
