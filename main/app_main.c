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
#include "mosu_time.h"
#include "mosu_ui.h"

static const char *TAG = "mosu";

static mosu_config_t s_cfg;

static bool s_have_ui;
static bool s_display_ok;
static volatile bool s_screen_on;
static bool s_lvgl_stopped;

static esp_pm_lock_handle_t s_display_lock;

static mosu_tap_t s_tap;
static portMUX_TYPE s_tap_mux = portMUX_INITIALIZER_UNLOCKED;
static mosu_wrist_t s_wrist;
static mosu_wrist_cfg_t s_wrist_cfg;

static volatile bool s_say_requested;
static volatile bool s_touch_seen;
static volatile uint32_t s_last_activity_ms;

static uint8_t s_battery_shown;
static bool s_battery_have;

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
 * LVGL reports the raw edges; the double tap detector decides whether the
 * watch should speak.  The callback runs in the LVGL task even while the face
 * is off, so s_tap is shared with the app task: guard it with a spinlock.
 *
 * The touch interrupt itself belongs to esp_lvgl_port: it registers the ISR
 * on the INT pin and puts the input device in event mode.  We must not add a
 * second handler on that pin.
 */
static void tap_reset(void)
{
    mosu_tap_cfg_t cfg = {
        .double_tap_ms = s_cfg.announce.double_tap_ms,
        .max_hold_ms = s_cfg.announce.max_hold_ms,
        .max_move_px = s_cfg.announce.max_move_px,
        .cooldown_ms = s_cfg.announce.cooldown_ms,
    };

    taskENTER_CRITICAL(&s_tap_mux);
    mosu_tap_init(&s_tap, &cfg);
    taskEXIT_CRITICAL(&s_tap_mux);
}

static void on_touch(bool pressed, int16_t x, int16_t y, void *user)
{
    (void)user;

    uint32_t stamp = now_ms();
    s_last_activity_ms = stamp;
    s_touch_seen = true;

    mosu_touch_sample_t sample = {
        .pressed = pressed,
        .x = x,
        .y = y,
        .t_ms = stamp,
    };

    taskENTER_CRITICAL(&s_tap_mux);
    bool fired = mosu_tap_update(&s_tap, &sample);
    taskEXIT_CRITICAL(&s_tap_mux);

    if (fired) {
        s_say_requested = true;
    }
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

    if (!pmu.battery_present) {
        s_battery_have = false;
        mosu_ui_set_battery(0, false);
        return;
    }

    if (!s_battery_have || immediate) {
        s_battery_shown = pmu.percent;
        s_battery_have = true;
    } else {
        /* Smooth the ADC noise so the colour does not flicker at a
         * threshold. */
        s_battery_shown = (uint8_t)((s_battery_shown * 3u + pmu.percent + 2u) /
                                    4u);
    }
    mosu_ui_set_battery(s_battery_shown, true);
}

static void update_wifi_icon(void)
{
    mosu_net_status_t status;
    mosu_net_status(&status);

    mosu_ui_wifi_t icon = MOSU_UI_WIFI_IDLE;
    if (status.state == MOSU_NET_FAILED) {
        icon = MOSU_UI_WIFI_FAIL;
    } else if (status.have_sync) {
        icon = MOSU_UI_WIFI_OK;
    }
    mosu_ui_set_wifi(icon);
}

static void screen_on(bool via_touch)
{
    if (s_screen_on) {
        s_last_activity_ms = now_ms();
        return;
    }

    if (s_display_ok) {
        mosu_board_display_on();
        /* Brightness is restored on every wake: switching the panel off
         * sets it to zero. */
        mosu_board_brightness_set(s_cfg.display.brightness);
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

    s_screen_on = true;
    s_last_activity_ms = now_ms();
    s_touch_seen = false;

    ESP_LOGD(TAG, "screen on (%s)", via_touch ? "touch" : "motion");
}

static void screen_off(void)
{
    if (!s_screen_on) {
        return;
    }

    if (s_have_ui) {
        lvgl_port_stop();
        s_lvgl_stopped = true;
    }
    if (s_display_ok) {
        mosu_board_display_off();
    }
    if (s_display_lock) {
        esp_pm_lock_release(s_display_lock);
    }

    s_screen_on = false;
    tap_reset();
    mosu_wrist_reset(&s_wrist);
    s_touch_seen = false;

    ESP_LOGD(TAG, "screen off");
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
    tap_reset();

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

    /* The codec is created once, then parked: enabling it on every
     * announcement would add a hiccup to the first word. */
    if (mosu_board_audio_init() == ESP_OK) {
        mosu_board_audio_idle();
    } else {
        ESP_LOGW(TAG, "audio unavailable, announcements will be silent");
    }
    mosu_audio_start(&s_cfg);

    mosu_board_enable_touch_wake(s_cfg.display.wake_on_touch);

    mosu_net_start(&s_cfg);
    mosu_diag_start(&s_cfg);

    if (!s_have_ui) {
        ESP_LOGE(TAG, "no watch face: running headless for diagnostics");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Show the time once at boot so the wearer knows the watch is alive,
     * then go back to the normal always-off behaviour. */
    if (s_have_ui) {
        screen_on(false);
    }

    uint32_t battery_poll = now_ms();
    uint32_t wifi_poll = 0;

    for (;;) {
        uint32_t now = now_ms();

        if (s_screen_on) {
            update_time();

            if (s_say_requested) {
                s_say_requested = false;
                speak_now();
                s_last_activity_ms = now_ms();
            }

            if (battery_poll == 0 || (uint32_t)(now - battery_poll) >= 60000u) {
                update_battery(false);
                battery_poll = now;
            }

            if (wifi_poll == 0 || (uint32_t)(now - wifi_poll) >= 2000u) {
                update_wifi_icon();
                wifi_poll = now;
            }

            /* Keep the face lit while it is speaking or while a finger is
             * resting on the glass. */
            if (mosu_audio_busy() || mosu_board_touch_active()) {
                s_last_activity_ms = now_ms();
            }

            if ((uint32_t)(now - s_last_activity_ms) >=
                s_cfg.display.screen_on_ms) {
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

        if (s_cfg.display.wake_on_touch &&
            (s_touch_seen || mosu_board_touch_active())) {
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
