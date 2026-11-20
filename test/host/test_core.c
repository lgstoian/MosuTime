/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Host-side tests for the pure C core and the INI parser.
 * Build:  cmake -S test/host -B test/host/build && cmake --build test/host/build
 * Run:    ./test/host/build/mosu_tests
 */
#include <math.h>
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

/* Gravity direction for a wrist that has turned `deg` from "arm down, glass
 * vertical" towards "glass facing the wearer".  The rotation is in the Y-Z
 * plane, so `deg` is also the angle the detector measures from the baseline. */
static mosu_accel_t wrist_tilt(float deg)
{
    const float r = deg * 3.14159265f / 180.0f;
    mosu_accel_t a = {0.0f, cosf(r), sinf(r)};
    return a;
}

/* Feed `duration_ms` of a held orientation, 20 Hz.  Returns the time a raise
 * fired, or 0. */
static uint32_t feed_hold(mosu_wrist_t *wrist, const mosu_wrist_cfg_t *cfg,
                          uint32_t t, float deg, uint32_t duration_ms)
{
    for (uint32_t elapsed = 0; elapsed < duration_ms; elapsed += 50) {
        mosu_accel_t a = wrist_tilt(deg);
        t += 50;
        if (mosu_wrist_update(wrist, &a, cfg, t)) {
            return t;
        }
    }
    return 0;
}

/* Feed a continuous raise: a linear ramp from 0 to `peak_deg` over `ramp_ms`,
 * then the orientation held for `hold_ms`.  This is what a real arm does and
 * what the old still-before gate could never accept. */
static uint32_t feed_raise(mosu_wrist_t *wrist, const mosu_wrist_cfg_t *cfg,
                           uint32_t t, float peak_deg, uint32_t ramp_ms,
                           uint32_t hold_ms)
{
    const uint32_t steps = ramp_ms / 50;
    for (uint32_t i = 1; i <= steps; i++) {
        mosu_accel_t a = wrist_tilt(peak_deg * (float)i / (float)steps);
        t += 50;
        if (mosu_wrist_update(wrist, &a, cfg, t)) {
            return t;
        }
    }
    return feed_hold(wrist, cfg, t, peak_deg, hold_ms);
}

static void test_wrist(void)
{
    mosu_wrist_cfg_t cfg = {.angle_mdeg = 45000, .refractory_ms = 2500};
    mosu_wrist_t wrist;

    /* A normal, continuous raise: 55 degrees over 300 ms.  The old detector
     * only fired when the arm jumped the threshold within one sample. */
    mosu_wrist_init(&wrist);
    uint32_t t = feed_hold(&wrist, &cfg, 0, 0.0f, 1000);
    EXPECT(t == 0);
    uint32_t fired = feed_raise(&wrist, &cfg, t, 55.0f, 300, 800);
    EXPECT(fired != 0);
    /* It must fire while the arm is at the look, not later. */
    EXPECT(fired - t <= 300 + 800);

    /* A raise that starts while the arm is still swinging (walking) also
     * fires: stillness is measured, but it is not a gate. */
    mosu_wrist_init(&wrist);
    t = 0;
    for (int i = 0; i < 40; i++) {
        mosu_accel_t a = wrist_tilt((i % 2) ? 12.0f : -12.0f);
        t += 50;
        EXPECT(!mosu_wrist_update(&wrist, &a, &cfg, t));
    }
    fired = feed_raise(&wrist, &cfg, t, 55.0f, 300, 800);
    EXPECT(fired != 0);

    /* A slow raise (50 degrees over 1.2 s) must not be absorbed into the
     * baseline while the arm is still turning. */
    mosu_wrist_init(&wrist);
    t = feed_hold(&wrist, &cfg, 0, 0.0f, 800);
    fired = feed_raise(&wrist, &cfg, t, 50.0f, 1200, 600);
    EXPECT(fired != 0);

    /* A reach that turns the wrist just as far but leaves the glass vertical
     * is not a glance at the time. */
    mosu_wrist_init(&wrist);
    t = feed_hold(&wrist, &cfg, 0, 0.0f, 1000);
    EXPECT(t == 0);
    t = 1000;
    for (int i = 1; i <= 8; i++) {
        mosu_accel_t a = {sinf((float)i / 8.0f * 1.5707963f), cosf((float)i / 8.0f * 1.5707963f), 0.0f};
        t += 50;
        EXPECT(!mosu_wrist_update(&wrist, &a, &cfg, t));
    }
    EXPECT(!mosu_wrist_update(&wrist, &(mosu_accel_t){1.0f, 0.0f, 0.0f}, &cfg, t + 50));

    /* Let the vertical (glass sideways) orientation settle and become the new
     * baseline, then turn the glass up: 90 degrees, ending with the screen
     * facing the wearer.  This is the recovery path that must not get stuck. */
    for (int i = 0; i < 80; i++) {
        mosu_accel_t a = {1.0f, 0.0f, 0.0f};
        t += 50;
        EXPECT(!mosu_wrist_update(&wrist, &a, &cfg, t));
    }
    uint32_t fire = 0;
    for (int i = 1; i <= 6; i++) {
        float r = (float)i / 6.0f * 1.5707963f;
        mosu_accel_t a = {cosf(r), 0.0f, sinf(r)};
        t += 50;
        if (mosu_wrist_update(&wrist, &a, &cfg, t)) {
            fire = t;
        }
    }
    for (int i = 0; i < 12 && !fire; i++) {
        mosu_accel_t a = {0.0f, 0.0f, 1.0f};
        t += 50;
        if (mosu_wrist_update(&wrist, &a, &cfg, t)) {
            fire = t;
        }
    }
    EXPECT(fire != 0);

    /* Holding the look after a raise does not retrigger. */
    mosu_wrist_init(&wrist);
    t = feed_hold(&wrist, &cfg, 0, 0.0f, 800);
    fired = feed_raise(&wrist, &cfg, t, 55.0f, 300, 800);
    EXPECT(fired != 0);
    EXPECT(!feed_hold(&wrist, &cfg, fired, 55.0f, 8000));

    /* The refractory window rejects a second raise. */
    mosu_wrist_init(&wrist);
    t = feed_hold(&wrist, &cfg, 0, 0.0f, 800);
    fired = feed_raise(&wrist, &cfg, t, 55.0f, 300, 800);
    EXPECT(fired != 0);
    /* Lower the arm and raise it again inside the refractory. */
    t = feed_hold(&wrist, &cfg, fired, 0.0f, 400);
    t = t ? t : fired + 400;
    EXPECT(feed_raise(&wrist, &cfg, t, 55.0f, 300, 800) == 0);

    /* Small wobble around the resting orientation never fires. */
    mosu_wrist_init(&wrist);
    t = feed_hold(&wrist, &cfg, 0, 0.0f, 800);
    for (int i = 0; i < 100; i++) {
        mosu_accel_t a = wrist_tilt((i % 2) ? 3.0f : -3.0f);
        t += 50;
        EXPECT(!mosu_wrist_update(&wrist, &a, &cfg, t));
    }

    /* An explicit, looser configuration is honoured at runtime. */
    mosu_wrist_cfg_t loose = {
        .angle_mdeg = 25000,
        .refractory_ms = 2500,
        .hold_ms = 150,
        .face_up_milli = 100,
    };
    mosu_wrist_init(&wrist);
    t = feed_hold(&wrist, &cfg, 0, 0.0f, 800);
    fired = feed_raise(&wrist, &loose, t, 30.0f, 200, 400);
    EXPECT(fired != 0);
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
