/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * MosuTime: a watch that shows a very large time, and says it out loud when
 * asked.  Everything here is about doing that with as little energy as
 * possible: the panel and the LVGL task are off most of the time, the CPU
 * sleeps between accelerometer samples, and the radio wakes up only to learn
 * the time and the timezone.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "mosu_audio.h"
#include "mosu_board.h"
#include "mosu_config.h"
#include "mosu_config_store.h"
#include "mosu_core/mosu_core.h"
#include "mosu_core/mosu_layout.h"
#include "mosu_diag.h"
#include "mosu_net.h"
#include "mosu_policy.h"
#include "mosu_power.h"
#include "mosu_session.h"
#include "mosu_time.h"
#include "mosu_ui.h"

_Static_assert((int)MOSU_SESSION_WIFI_IDLE == (int)MOSU_UI_WIFI_IDLE &&
                   (int)MOSU_SESSION_WIFI_OK == (int)MOSU_UI_WIFI_OK &&
                   (int)MOSU_SESSION_WIFI_FAIL == (int)MOSU_UI_WIFI_FAIL,
               "session and UI Wi-Fi enums must match");
_Static_assert((int)MOSU_SESSION_NET_IDLE == (int)MOSU_NET_IDLE &&
                   (int)MOSU_SESSION_NET_CONNECTING ==
                       (int)MOSU_NET_CONNECTING &&
                   (int)MOSU_SESSION_NET_CONNECTED == (int)MOSU_NET_CONNECTED &&
                   (int)MOSU_SESSION_NET_FAILED == (int)MOSU_NET_FAILED,
               "session and net state enums must match");

static const char *TAG = "mosu";

static mosu_config_t s_cfg;

static bool s_have_ui;
static bool s_display_ok;
static bool s_lvgl_stopped;

/* Power accounting for the `stats` console command.  Screen-on seconds and
 * wake count are the two numbers that explain a short battery life.  They
 * live in RTC memory so opening the USB console (which resets the CPU) does
 * not wipe a measurement that started on battery. */
/* Bump when the RTC_NOINIT layout changes: the magic only says "this block was
 * written by this firmware", and reusing it across a layout change would
 * interpret old bytes as new counters. */
#define MOSU_STATS_MAGIC 0x4D4F5356u
/* RTC_NOINIT, not RTC_DATA: a plain CPU reset (the USB console opening does
 * one) re-initialises .rtc.data, while .rtc.noinit survives it. */
RTC_NOINIT_ATTR static uint32_t s_stats_magic;
RTC_NOINIT_ATTR static uint32_t s_wake_count;
RTC_NOINIT_ATTR static uint32_t s_screen_on_total_ms;
RTC_NOINIT_ATTR static uint32_t s_baseline_epoch;
RTC_NOINIT_ATTR static uint8_t s_boot_pct;
RTC_NOINIT_ATTR static bool s_boot_pct_set;
RTC_NOINIT_ATTR static uint32_t s_boot_count;
RTC_NOINIT_ATTR static uint32_t s_raise_fired;
RTC_NOINIT_ATTR static uint32_t s_raise_candidates;
RTC_NOINIT_ATTR static int16_t s_last_raise_nz_milli;
RTC_NOINIT_ATTR static uint8_t s_last_raise_deg;
static uint32_t s_screen_on_since_ms;

/* Learned screen policy: it adapts the wrist threshold and the lit window to
 * how often the wearer actually looks at the watch. */
static mosu_power_learn_t s_power_learn;
static uint32_t s_power_last_ms;
static uint32_t s_power_window_start;

/* A record in flash survives a dead cell, unlike RTC_NOINIT.  Written every
 * five minutes so an overnight run still tells us the drain rate. */
typedef struct {
    uint32_t epoch;
    uint32_t start_epoch;
    uint16_t mv;
    uint8_t pct;
    uint8_t start_pct;
} mosu_pwrlog_t;

static mosu_pwrlog_t s_last_log;

static void power_log_restore(void)
{
    nvs_handle_t handle;
    if (nvs_open("mosu", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_last_log);
    nvs_get_blob(handle, "pwrlog", &s_last_log, &len);
    nvs_close(handle);
}

static void power_log_store(void)
{
    mosu_board_pmu_t pmu;
    if (mosu_board_pmu_read(&pmu) != ESP_OK || !pmu.battery_present ||
        pmu.charging) {
        return;
    }
    time_t now = time(NULL);
    if (now < 1600000000) {
        return;
    }
    if (s_last_log.epoch == 0 || pmu.percent >= s_last_log.pct) {
        /* First sample after a charge: start a new discharge run. */
        s_last_log.start_epoch = (uint32_t)now;
        s_last_log.start_pct = pmu.percent;
    }
    s_last_log.epoch = (uint32_t)now;
    s_last_log.mv = pmu.millivolts;
    s_last_log.pct = pmu.percent;
    nvs_handle_t handle;
    if (nvs_open("mosu", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_blob(handle, "pwrlog", &s_last_log, sizeof(s_last_log));
    nvs_commit(handle);
    nvs_close(handle);
}

/* Append one sample per minute to /sdcard/power.log.  The card is
 * non-volatile, so a full discharge from 100% to empty leaves the whole curve
 * behind even when RTC memory is lost. */
static void power_log_sd(void)
{
    if (!mosu_board_sdcard_ready()) {
        return;
    }
    mosu_board_pmu_t pmu;
    if (mosu_board_pmu_read(&pmu) != ESP_OK || !pmu.battery_present) {
        return;
    }
    FILE *file = fopen("/sdcard/power.log", "a");
    if (!file) {
        return;
    }
    const bool on = mosu_power_screen_is_on();
    fprintf(file, "%ld,%u,%u,%d,%d,%u,%u\n", (long)time(NULL),
            (unsigned)pmu.percent, (unsigned)pmu.millivolts,
            pmu.charging ? 1 : 0, on ? 1 : 0,
            (unsigned)s_wake_count, (unsigned)s_boot_count);
    fclose(file);
}

static void stats_init(void)
{
    if (s_stats_magic == MOSU_STATS_MAGIC) {
        s_boot_count++;
        return;
    }
    s_stats_magic = MOSU_STATS_MAGIC;
    s_wake_count = 0;
    s_screen_on_total_ms = 0;
    s_raise_fired = 0;
    s_raise_candidates = 0;
    s_last_raise_nz_milli = 0;
    s_last_raise_deg = 0;
    s_baseline_epoch = 0;
    s_boot_pct = 0;
    s_boot_pct_set = false;
    s_boot_count = 1;
}

static mosu_session_t s_session;
static portMUX_TYPE s_session_mux = portMUX_INITIALIZER_UNLOCKED;
static mosu_wrist_t s_wrist;
static mosu_wrist_cfg_t s_wrist_cfg;
/* Audit kill switches for the one-hour A/B discharge runs. */
static bool s_audit_imu = true;
static bool s_audit_touch = true;
static bool s_touch_wake_applied = true;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------ logging */

static esp_log_level_t log_level_for(uint8_t level)
{
    switch (level) {
    case 0:
        return ESP_LOG_NONE;
    case 1:
        return ESP_LOG_ERROR;
    case 2:
        return ESP_LOG_WARN;
    case 4:
        return ESP_LOG_DEBUG;
    case 5:
        return ESP_LOG_VERBOSE;
    default:
        return ESP_LOG_INFO;
    }
}

/* -------------------------------------------------------------------- touch */

/*
 * The touch interrupt belongs to esp_lvgl_port: it registers the ISR on the
 * INT pin and puts the input device in event mode.  We must not add a second
 * handler on that pin.
 *
 * LVGL reports the raw edges; mosu_session owns the double tap detector and
 * the wake request.  The callback runs in the LVGL task even while the face is
 * off (only the LVGL timers are stopped), so the session is shared with the
 * app task and guarded by s_session_mux.
 */
static void on_touch(bool pressed, int16_t x, int16_t y, void *user)
{
    (void)user;

    mosu_touch_sample_t sample = {
        .pressed = pressed,
        .x = x,
        .y = y,
        .t_ms = now_ms(),
    };

    if (mosu_diag_touch_test_active()) {
        ESP_LOGI(TAG, "touch %s x=%d y=%d", pressed ? "down" : "up", x, y);
    }

    taskENTER_CRITICAL(&s_session_mux);
    mosu_session_touch(&s_session, &sample);
    taskEXIT_CRITICAL(&s_session_mux);
}

/* ------------------------------------------------------------------ display */

static void update_time(void)
{
    struct tm local;
    mosu_time_now(&local);
    mosu_ui_set_time((uint8_t)local.tm_hour, (uint8_t)local.tm_min,
                     mosu_time_valid());
}

static void update_battery(bool immediate)
{
    mosu_board_pmu_t pmu;
    if (mosu_board_pmu_read(&pmu) != ESP_OK) {
        return;
    }

    uint8_t shown = mosu_session_battery(&s_session, pmu.percent,
                                         pmu.battery_present, immediate);
    if (!s_boot_pct_set && pmu.battery_present) {
        s_boot_pct = pmu.percent;
        s_boot_pct_set = true;
    }
    if (s_baseline_epoch == 0) {
        time_t now = time(NULL);
        if (now > 1600000000) {
            s_baseline_epoch = (uint32_t)now;
        }
    }
    mosu_ui_set_battery(shown, pmu.battery_present);
}

static void update_wifi_icon(void)
{
    const uint32_t interval =
        s_cfg.clock.sync_per_day > 0
            ? 86400u / s_cfg.clock.sync_per_day
            : (uint32_t)s_cfg.clock.sync_hours * 3600u;
    mosu_ui_set_wifi((mosu_ui_wifi_t)mosu_session_wifi_from_age(
        mosu_time_sync_age_s(), interval));
}

static void screen_on(bool via_touch)
{
    if (mosu_session_is_on(&s_session)) {
        taskENTER_CRITICAL(&s_session_mux);
        mosu_session_activity(&s_session, now_ms());
        taskEXIT_CRITICAL(&s_session_mux);
        return;
    }

    /* The panel keeps its last frame while the display is off, so switching
     * it on shows the clock immediately; the LVGL task redraws afterwards.
     * The power manager owns the panel, brightness and the sleep lock. */
    if (s_display_ok) {
        mosu_power_screen_on();
    }

    if (s_have_ui) {
        update_time();
        update_battery(true);
        update_wifi_icon();
        if (s_lvgl_stopped) {
            lvgl_port_resume();
            s_lvgl_stopped = false;
        }
        /* Only invalidate: the LVGL task owns rendering.  Rendering from this
         * task while holding the port lock returned mid-frame and overflowed
         * the 3.5 KB app stack. */
        mosu_ui_refresh();
    }

    taskENTER_CRITICAL(&s_session_mux);
    mosu_session_screen_on(&s_session, now_ms());
    taskEXIT_CRITICAL(&s_session_mux);
    if (s_cfg.device.stats) {
        s_wake_count++;
        s_screen_on_since_ms = now_ms();
    }
    mosu_power_learn_wake(&s_power_learn, via_touch);

    ESP_LOGI(TAG, "screen on (%s)", via_touch ? "touch" : "motion");
}

static void screen_off(void)
{
    if (!mosu_session_is_on(&s_session)) {
        return;
    }

    if (s_have_ui) {
        lvgl_port_stop();
        s_lvgl_stopped = true;
        /* Let an in-flight QSPI flush finish before the panel is switched
         * off and put to sleep.  A full frame is ~411 KB, so 80 ms is the
         * safe side of the transfer time. */
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    if (s_display_ok) {
        mosu_power_screen_off();
        mosu_board_enable_touch_wake(s_cfg.display.wake_on_touch);
    }

    taskENTER_CRITICAL(&s_session_mux);
    mosu_session_screen_off(&s_session, now_ms());
    taskEXIT_CRITICAL(&s_session_mux);
    if (s_cfg.device.stats) {
        s_screen_on_total_ms += now_ms() - s_screen_on_since_ms;
    }
    mosu_wrist_reset(&s_wrist);

    ESP_LOGI(TAG, "screen off");
}

/* ------------------------------------------------------------------- speech */

static void speak_now(void)
{
    mosu_ui_flash_tap();

    if (!s_cfg.announce.enabled) {
        return;
    }

    struct tm local;
    mosu_time_now(&local);

    if (!mosu_time_valid()) {
        /* A short, gentle signal that the time is not known yet; silence
         * would look like a fault. */
        if (!mosu_audio_busy()) {
            mosu_board_audio_init();
            mosu_board_audio_beep(660, 140);
            mosu_board_audio_idle();
        }
        return;
    }

    mosu_phrase_t phrase;
    mosu_phrase_build((uint8_t)local.tm_hour, (uint8_t)local.tm_min,
                      mosu_weekday_from_tm(local.tm_wday),
                      s_cfg.announce.announce_day, &phrase);
    if (!mosu_audio_say(&phrase)) {
        ESP_LOGW(TAG, "announcement queue rejected the request");
    }
}

/* --------------------------------------------------------------------- boot */

static void load_config(void)
{
    mosu_config_defaults(&s_cfg);

    bool from_sd = false;
    if (mosu_board_sdcard_mount() == ESP_OK) {
        mosu_config_report_t report;
        if (mosu_config_load(&s_cfg, MOSU_CFG_PATH, &report) == ESP_OK) {
            from_sd = true;
            ESP_LOGI(TAG, "config: %d lines, %d applied, %d warnings, %d errors",
                     report.lines, report.applied, report.warnings,
                     report.errors);
            if (report.message[0] != '\0') {
                ESP_LOGW(TAG, "config: %s", report.message);
            }
        }
    }

    if (!from_sd) {
        if (mosu_config_store_load(&s_cfg)) {
            ESP_LOGW(TAG, "using the cached config, SD card unavailable");
        } else {
            ESP_LOGW(TAG, "using built-in defaults");
        }
    }

    mosu_config_sanitize(&s_cfg);

    if (from_sd) {
        mosu_config_store_save(&s_cfg);
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }
}

/* ---------------------------------------------------------------- console */

/*
 * The console runs in its own task.  It never touches LVGL, the PMU or the
 * session directly: it leaves a request here and the app task performs it on
 * its next pass, where screen_on()/screen_off() are safe to call.
 */
static volatile int s_diag_screen_request;   /* 0 none, 1 on, 2 off */
static volatile int s_diag_pattern_request;  /* -1 none, else mosu_ui_test_t */
static volatile bool s_diag_wifi_request;
static bool s_test_pattern_active;
static bool s_force_screen_on;

static void diag_screen_on(void)
{
    s_force_screen_on = true;
    s_diag_screen_request = 1;
}

/* Wake for one normal activity window; do not hold the panel on. */
static void diag_wake(void)
{
    s_diag_screen_request = 1;
}

static void diag_screen_off(void)
{
    s_force_screen_on = false;
    s_test_pattern_active = false;
    s_diag_screen_request = 2;
}

static void diag_test_pattern(int pattern)
{
    s_force_screen_on = false;
    s_diag_pattern_request = pattern;
    s_diag_screen_request = 1;
}

static void diag_power_wifi(void)
{
    s_diag_wifi_request = true;
}

static void diag_power_idle(void)
{
    s_force_screen_on = false;
    s_test_pattern_active = false;
    s_diag_pattern_request = (int)MOSU_UI_TEST_NONE;
}

static void apply_power_learning(void)
{
    uint16_t angle = mosu_power_learn_angle(&s_power_learn,
                                            s_cfg.display.motion_angle_deg);
    s_wrist_cfg.angle_mdeg = angle * 1000u;
    s_wrist_cfg.hold_ms = mosu_power_learn_hold(&s_power_learn, 250);
    s_wrist_cfg.face_up_milli = mosu_power_learn_face(&s_power_learn, 200);

    uint32_t screen =
        mosu_power_learn_screen_ms(&s_power_learn, s_cfg.display.screen_on_ms);
    taskENTER_CRITICAL(&s_session_mux);
    mosu_session_set_screen_on_ms(&s_session, screen);
    taskEXIT_CRITICAL(&s_session_mux);

    ESP_LOGI(TAG, "auto power: raise %u deg/%u ms, face %u, screen %u ms",
             (unsigned)angle, (unsigned)s_wrist_cfg.hold_ms,
             (unsigned)s_wrist_cfg.face_up_milli, (unsigned)screen);
}

static void diag_stats(mosu_diag_stats_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_cfg.device.stats) {
        return;
    }

    time_t now = time(NULL);
    uint32_t elapsed = 0;
    if (s_baseline_epoch && now > (time_t)s_baseline_epoch) {
        elapsed = (uint32_t)(now - (time_t)s_baseline_epoch);
    }
    if (elapsed == 0) {
        elapsed = now_ms() / 1000u;
    }
    out->uptime_s = elapsed;
    out->wakes = s_wake_count;
    out->boots = s_boot_count;
    out->boot_pct = s_boot_pct_set ? s_boot_pct : 0;

    uint32_t on_ms = s_screen_on_total_ms;
    if (mosu_session_is_on(&s_session)) {
        on_ms += now_ms() - s_screen_on_since_ms;
    }
    out->screen_on_s = on_ms / 1000u;
    out->screen_ms = mosu_power_learn_screen_ms(&s_power_learn,
                                                s_cfg.display.screen_on_ms);
    out->hold_ms = mosu_power_learn_hold(&s_power_learn, 250);
    out->face_milli = mosu_power_learn_face(&s_power_learn, 200);
    out->raise_fired = s_raise_fired;
    out->raise_candidates = s_raise_candidates;
    out->wifi_on_s = mosu_net_wifi_on_ms() / 1000u;
    out->audio_on_s = mosu_audio_on_ms() / 1000u;
    out->last_raise_deg = s_last_raise_deg;
    out->last_nz_milli = s_last_raise_nz_milli;
    out->angle_deg = (uint8_t)mosu_power_learn_angle(
        &s_power_learn, s_cfg.display.motion_angle_deg);

    mosu_board_pmu_t pmu;
    if (mosu_board_pmu_read(&pmu) == ESP_OK) {
        out->battery_present = pmu.battery_present;
        out->charging = pmu.charging;
        out->battery_mv = pmu.millivolts;
        out->battery_pct = pmu.percent;
    }
}

static void diag_stats_reset(void)
{
    if (!s_cfg.device.stats) {
        return;
    }
    mosu_board_pmu_t pmu;
    if (mosu_board_pmu_read(&pmu) != ESP_OK || !pmu.battery_present) {
        return;
    }
    time_t now = time(NULL);
    s_baseline_epoch = now > 1600000000 ? (uint32_t)now : 0;
    s_boot_pct = pmu.percent;
    s_boot_pct_set = true;
    s_wake_count = 0;
    s_screen_on_total_ms = 0;
    s_raise_fired = 0;
    s_raise_candidates = 0;
    s_last_raise_deg = 0;
    s_last_raise_nz_milli = 0;
    s_screen_on_since_ms = now_ms();
}

static bool diag_power_log(mosu_power_log_t *out)
{
    if (!out || !s_last_log.epoch) {
        return false;
    }
    *out = (mosu_power_log_t){
        .epoch = s_last_log.epoch,
        .start_epoch = s_last_log.start_epoch,
        .mv = s_last_log.mv,
        .pct = s_last_log.pct,
        .start_pct = s_last_log.start_pct,
    };
    return true;
}

static void diag_audit(const char *args)
{
    char name[16] = {0};
    char state[8] = {0};

    if (args && *args) {
        if (sscanf(args, "%15s %7s", name, state) != 2 ||
            (strcmp(state, "on") && strcmp(state, "off"))) {
            printf("usage: audit <wifi|imu|touch|audio|panel> on|off\n");
            return;
        }
        const bool on = !strcmp(state, "on");
        if (!strcmp(name, "wifi")) {
            mosu_net_set_enabled(on);
        } else if (!strcmp(name, "audio")) {
            mosu_audio_set_enabled(on);
        } else if (!strcmp(name, "imu")) {
            s_audit_imu = on;
        } else if (!strcmp(name, "touch")) {
            /* The app task applies the GPIO wake state on its next pass. */
            s_audit_touch = on;
        } else if (!strcmp(name, "panel")) {
            mosu_board_display_set_panel_sleep(on);
        } else {
            printf("audit: unknown subsystem %s\n", name);
            return;
        }
    }

    printf("audit: wifi %s, imu %s, touch %s, audio %s, panel sleep %s\n",
           mosu_net_enabled() ? "on" : "off",
           s_audit_imu ? "on" : "off", s_audit_touch ? "on" : "off",
           mosu_audio_enabled() ? "on" : "off",
           mosu_board_display_panel_sleep() ? "on" : "off");
    printf("audit: radio on %u s, audio on %u s (since boot)\n",
           (unsigned)(mosu_net_wifi_on_ms() / 1000u),
           (unsigned)(mosu_audio_on_ms() / 1000u));
}

static const mosu_diag_hooks_t s_diag_hooks = {
    .wake = diag_wake,
    .screen_on = diag_screen_on,
    .screen_off = diag_screen_off,
    .test_pattern = diag_test_pattern,
    .power_wifi = diag_power_wifi,
    .power_idle = diag_power_idle,
    .stats = diag_stats,
    .stats_reset = diag_stats_reset,
    .audit = diag_audit,
    .power_log = diag_power_log,
};

static void handle_diag_requests(void)
{
    int screen = s_diag_screen_request;
    s_diag_screen_request = 0;
    if (screen == 1) {
        screen_on(false);
    } else if (screen == 2) {
        screen_off();
    }

    int pattern = s_diag_pattern_request;
    s_diag_pattern_request = -1;
    if (pattern >= 0) {
        s_test_pattern_active = pattern != (int)MOSU_UI_TEST_NONE;
        if (!mosu_session_is_on(&s_session)) {
            screen_on(false);
        }
        if (s_have_ui) {
            mosu_ui_test_pattern((mosu_ui_test_t)pattern);
        }
        taskENTER_CRITICAL(&s_session_mux);
        mosu_session_activity(&s_session, now_ms());
        taskEXIT_CRITICAL(&s_session_mux);
    }

    if (s_diag_wifi_request) {
        s_diag_wifi_request = false;
        mosu_net_request_sync();
    }
}

void app_main(void)
{
    stats_init();
    init_nvs();
    power_log_restore();

    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set("mosu", ESP_LOG_ERROR);

    esp_err_t err = mosu_board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    load_config();
    esp_log_level_set("*", log_level_for(s_cfg.device.log_level));
    mosu_board_display_set_panel_sleep(s_cfg.display.panel_sleep);

    s_wrist_cfg.angle_mdeg =
        (uint16_t)(s_cfg.display.motion_angle_deg * 1000u);
    s_wrist_cfg.refractory_ms = s_cfg.display.motion_refractory_ms;
    mosu_wrist_init(&s_wrist);

    mosu_session_cfg_t session_cfg = {
        .screen_on_ms = s_cfg.display.screen_on_ms,
        .tap =
            {
                .double_tap_ms = s_cfg.announce.double_tap_ms,
                .max_hold_ms = s_cfg.announce.max_hold_ms,
                .max_move_px = s_cfg.announce.max_move_px,
                .cooldown_ms = s_cfg.announce.cooldown_ms,
            },
        .battery_green_min = s_cfg.status.battery_green_min,
        .battery_orange_min = s_cfg.status.battery_orange_min,
    };
    mosu_session_init(&s_session, &session_cfg);

    ESP_LOGI(TAG, "starting display");
    err = mosu_board_display_start();
    if (err == ESP_OK) {
        s_display_ok = true;
        /* The power manager applies the configured brightness and the
         * CPU/light-sleep policy before anything is drawn. */
        mosu_power_init(&s_cfg);
        err = mosu_ui_create(&s_cfg);
        if (err == ESP_OK) {
            s_have_ui = true;
            mosu_ui_set_touch_cb(on_touch, NULL);
        } else {
            ESP_LOGE(TAG, "UI init failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
    }

    mosu_time_init(&s_cfg);

    /* Face first, radio later: the first glance should not wait for Wi-Fi
     * and the announcement task to come up. */
    if (s_have_ui) {
        screen_on(false);
    }

    /* The codec is created once, then parked: enabling it on every
     * announcement would add a hiccup to the first word. */
    if (mosu_board_audio_init() == ESP_OK) {
        mosu_board_audio_idle();
    } else {
        ESP_LOGW(TAG, "audio unavailable, announcements will be silent");
    }
    mosu_audio_start(&s_cfg);

    mosu_net_start(&s_cfg);
    mosu_diag_set_hooks(&s_diag_hooks);
    mosu_diag_start(&s_cfg);

    /* On ESP32-S3 the USB-Serial/JTAG PHY keeps its oscillator running even
     * with no cable attached, which prevents automatic light sleep.  When we
     * boot on the cell, power the pad down.  Plugging a cable in resets the
     * chip and the ROM restores it, so the console still works. */
    mosu_board_pmu_t boot_pmu;
    if (mosu_board_pmu_read(&boot_pmu) == ESP_OK && !boot_pmu.vbus_present) {
        CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG,
                            USB_SERIAL_JTAG_USB_PAD_ENABLE_M);
        ESP_LOGI(TAG, "USB PHY powered down (battery boot)");
    }

    if (!s_have_ui) {
        ESP_LOGE(TAG, "no watch face: running headless for diagnostics");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    uint32_t battery_poll = now_ms();
    uint32_t wifi_poll = 0;
    uint32_t clock_poll = 0;
    uint32_t pwrlog_poll = now_ms();
    uint32_t sdlog_poll = now_ms();

    power_log_sd();

    mosu_power_learn_init(&s_power_learn);
    s_power_last_ms = now_ms();
    s_power_window_start = s_power_last_ms;

    for (;;) {
        handle_diag_requests();
        /* Audit touch switch: apply only on change, on the task that owns the
         * touch GPIO.  Calling the driver from the console task hung the
         * board. */
        if (s_audit_touch != s_touch_wake_applied) {
            s_touch_wake_applied = s_audit_touch;
            if (s_display_ok) {
                mosu_board_enable_touch_wake(
                    s_touch_wake_applied && s_cfg.display.wake_on_touch);
            }
        }
        uint32_t now = now_ms();

        if ((uint32_t)(now - pwrlog_poll) >= 300000u) {
            power_log_store();
            pwrlog_poll = now;
        }
        if (sdlog_poll == 0 || (uint32_t)(now - sdlog_poll) >= 60000u) {
            power_log_sd();
            sdlog_poll = now;
        }

        uint32_t dt = now - s_power_last_ms;
        s_power_last_ms = now;
        mosu_power_learn_note(&s_power_learn, dt,
                              mosu_session_is_on(&s_session));
        if ((uint32_t)(now - s_power_window_start) >= 3600000u) {
            s_power_window_start = now;
            if (mosu_power_learn_tick(&s_power_learn, 3600000u)) {
                apply_power_learning();
            }
        }

        if (mosu_session_is_on(&s_session)) {
            /* The face only changes on the minute; reading the RTC ten times a
             * second buys nothing and keeps an I2C transaction in the loop. */
            if (clock_poll == 0 || (uint32_t)(now - clock_poll) >= 1000u) {
                update_time();
                clock_poll = now;
            }

            bool say;
            taskENTER_CRITICAL(&s_session_mux);
            say = mosu_session_take_say(&s_session);
            taskEXIT_CRITICAL(&s_session_mux);
            if (say) {
                speak_now();
                /* Speaking earns a full window after the sentence. */
                taskENTER_CRITICAL(&s_session_mux);
                mosu_session_activity(&s_session, now_ms());
                taskEXIT_CRITICAL(&s_session_mux);
            }

            if (battery_poll == 0 || (uint32_t)(now - battery_poll) >= 60000u) {
                update_battery(false);
                battery_poll = now;
            }

            if (wifi_poll == 0 || (uint32_t)(now - wifi_poll) >= 5000u) {
                update_wifi_icon();
                wifi_poll = now;
            }

            /* Reading the two hardware booleans is slower than the critical
             * section itself, so do it before taking the spinlock. */
            bool audio = mosu_audio_busy();
            bool touching = mosu_board_touch_active() || s_test_pattern_active ||
                            s_force_screen_on;
            taskENTER_CRITICAL(&s_session_mux);
            bool sleep_now =
                mosu_session_should_sleep(&s_session, now, audio, touching);
            taskEXIT_CRITICAL(&s_session_mux);
            if (sleep_now) {
                screen_off();
            }

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Screen off: sleep until the next accelerometer sample, the touch
         * interrupt, or the poll interval, whichever comes first. */
        uint32_t poll_ms = 1000u / s_cfg.display.motion_poll_hz;
        if (poll_ms < 50u) {
            poll_ms = 50u;
        }

        vTaskDelay(pdMS_TO_TICKS(poll_ms));

        bool touched;
        taskENTER_CRITICAL(&s_session_mux);
        touched = mosu_session_take_wake(&s_session);
        taskEXIT_CRITICAL(&s_session_mux);

        if (s_audit_touch && s_cfg.display.wake_on_touch &&
            (touched || mosu_board_touch_active())) {
            screen_on(true);
            continue;
        }

        if (!s_audit_imu || !s_cfg.display.wake_on_motion) {
            continue;
        }

        mosu_board_accel_t accel;
        if (mosu_board_imu_read(&accel) != ESP_OK) {
            continue;
        }

        mosu_accel_t sample = {accel.x, accel.y, accel.z};
        if (mosu_wrist_update(&s_wrist, &sample, &s_wrist_cfg, now_ms())) {
            if (s_cfg.device.stats) {
                s_raise_fired++;
                s_raise_candidates = s_wrist.candidates;
                s_last_raise_deg = (uint8_t)(s_wrist.last_angle_deg + 0.5f);
                s_last_raise_nz_milli = (int16_t)(s_wrist.last_nz * 1000.0f);
            }
            ESP_LOGI(TAG, "raise: %.0f deg, nz %.2f, %u candidates",
                     (double)s_wrist.last_angle_deg, (double)s_wrist.last_nz,
                     (unsigned)s_wrist.candidates);
            screen_on(false);
        }
    }
}
