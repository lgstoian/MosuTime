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
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
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

static esp_pm_lock_handle_t s_display_lock;

static mosu_session_t s_session;
static portMUX_TYPE s_session_mux = portMUX_INITIALIZER_UNLOCKED;
static mosu_wrist_t s_wrist;
static mosu_wrist_cfg_t s_wrist_cfg;

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
    mosu_ui_set_battery(shown, pmu.battery_present);
}

static void update_wifi_icon(void)
{
    mosu_net_status_t status;
    mosu_net_status(&status);

    mosu_ui_set_wifi((mosu_ui_wifi_t)mosu_session_wifi_icon(
        (mosu_session_net_state_t)status.state, status.have_sync));
}

static void screen_on(bool via_touch)
{
    if (mosu_session_is_on(&s_session)) {
        taskENTER_CRITICAL(&s_session_mux);
        mosu_session_activity(&s_session, now_ms());
        taskEXIT_CRITICAL(&s_session_mux);
        return;
    }

    if (s_display_ok) {
        mosu_board_display_on();
        /* Brightness is restored on every wake: switching the panel off
         * sets it to zero. */
        mosu_board_brightness_set(s_cfg.display.brightness);
        mosu_board_enable_touch_wake(false);
    }

    if (s_display_lock) {
        esp_pm_lock_acquire(s_display_lock);
    }

    if (s_have_ui) {
        update_time();
        update_battery(true);
        update_wifi_icon();
        mosu_ui_refresh();
        if (s_lvgl_stopped) {
            lvgl_port_resume();
            s_lvgl_stopped = false;
        }
    }

    taskENTER_CRITICAL(&s_session_mux);
    mosu_session_screen_on(&s_session, now_ms());
    taskEXIT_CRITICAL(&s_session_mux);

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
    }
    if (s_display_ok) {
        mosu_board_display_off();
        mosu_board_enable_touch_wake(s_cfg.display.wake_on_touch);
    }
    if (s_display_lock) {
        esp_pm_lock_release(s_display_lock);
    }

    taskENTER_CRITICAL(&s_session_mux);
    mosu_session_screen_off(&s_session, now_ms());
    taskEXIT_CRITICAL(&s_session_mux);
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

static const mosu_diag_hooks_t s_diag_hooks = {
    .screen_on = diag_screen_on,
    .screen_off = diag_screen_off,
    .test_pattern = diag_test_pattern,
    .power_wifi = diag_power_wifi,
    .power_idle = diag_power_idle,
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
    init_nvs();

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mosu", ESP_LOG_INFO);

    esp_err_t err = mosu_board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s", esp_err_to_name(err));
    }

    load_config();
    esp_log_level_set("*", log_level_for(s_cfg.device.log_level));

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

    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 80,
        .light_sleep_enable = true,
    };
    err = esp_pm_configure(&pm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management: %s", esp_err_to_name(err));
    }
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "mosu_display",
                           &s_display_lock) != ESP_OK) {
        s_display_lock = NULL;
    }

    ESP_LOGI(TAG, "starting display");
    err = mosu_board_display_start();
    if (err == ESP_OK) {
        s_display_ok = true;
        /* Apply the configured brightness before anything is drawn. */
        mosu_board_brightness_set(s_cfg.display.brightness);
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

    if (!s_have_ui) {
        ESP_LOGE(TAG, "no watch face: running headless for diagnostics");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    uint32_t battery_poll = now_ms();
    uint32_t wifi_poll = 0;

    for (;;) {
        handle_diag_requests();
        uint32_t now = now_ms();

        if (mosu_session_is_on(&s_session)) {
            update_time();

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

            if (wifi_poll == 0 || (uint32_t)(now - wifi_poll) >= 2000u) {
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

        if (s_cfg.display.wake_on_touch &&
            (touched || mosu_board_touch_active())) {
            screen_on(true);
            continue;
        }

        if (!s_cfg.display.wake_on_motion) {
            continue;
        }

        mosu_board_accel_t accel;
        if (mosu_board_imu_read(&accel) != ESP_OK) {
            continue;
        }

        mosu_accel_t sample = {accel.x, accel.y, accel.z};
        if (mosu_wrist_update(&s_wrist, &sample, &s_wrist_cfg, now_ms())) {
            screen_on(false);
        }
    }
}
