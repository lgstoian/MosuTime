/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics over the console UART.  This is deliberately tiny: a handful of
 * commands that answer "is it synced, is the card there, does it speak".
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mosu_audio.h"
#include "mosu_board.h"
#include "mosu_config_store.h"
#include "mosu_core/mosu_core.h"
#include "mosu_diag.h"
#include "mosu_net.h"
#include "mosu_time.h"
#include "mosu_ui.h"

static const char *TAG = "mosu_diag";
static mosu_config_t *s_cfg;

static mosu_diag_hooks_t s_hooks;
static volatile bool s_touch_test;

static void print_status(void)
{
    mosu_net_status_t net;
    mosu_net_status(&net);

    mosu_board_pmu_t pmu;
    bool pmu_ok = mosu_board_pmu_read(&pmu) == ESP_OK;

    printf("config : %s (%s)\n", s_cfg->device.name,
           mosu_board_sdcard_ready() ? "SD mounted" : "no SD card");
    printf("clock  : %s, tz %s, sync age %s\n",
           mosu_time_valid() ? "valid" : "NOT SET", mosu_time_timezone(),
           mosu_time_sync_age_s() == UINT32_MAX ? "never" : "recent");
    printf("net    : state %d, ip %s, rssi %d\n", (int)net.state, net.ip,
           net.rssi);
    if (pmu_ok) {
        printf("power  : %u mV, %u%%, charging %d, vbus %d\n",
               (unsigned)pmu.millivolts, (unsigned)pmu.percent,
               (int)pmu.charging, (int)pmu.vbus_present);
    } else {
        printf("power  : PMU read failed\n");
    }
    printf("audio  : %s\n", mosu_audio_busy() ? "speaking" : "idle");
    printf("display: brightness %u%%\n", (unsigned)s_cfg->display.brightness);
}

/*
 * Every value that shapes the watch, except the Wi-Fi passwords.  The console
 * output ends up in issue reports and chat logs; a password must never be one
 * of those lines, not even when it is short enough to read at a glance.
 */
static void print_config(void)
{
    const mosu_config_t *c = s_cfg;

    printf("device : %s, log level %u\n", c->device.name,
           (unsigned)c->device.log_level);

    printf("display: brightness %u%%, screen on %u ms\n",
           (unsigned)c->display.brightness,
           (unsigned)c->display.screen_on_ms);
    printf("display: wake touch %d, motion %d, angle %u deg, refractory %u ms\n",
           (int)c->display.wake_on_touch, (int)c->display.wake_on_motion,
           (unsigned)c->display.motion_angle_deg,
           (unsigned)c->display.motion_refractory_ms);
    printf("display: poll %u Hz, pixel shift %u s, tap feedback %u ms\n",
           (unsigned)c->display.motion_poll_hz,
           (unsigned)c->display.pixel_shift_sec,
           (unsigned)c->display.tap_feedback_ms);

    printf("status : battery green >= %u, orange >= %u, show %d, wifi %d, "
           "slash %d\n",
           (unsigned)c->status.battery_green_min,
           (unsigned)c->status.battery_orange_min,
           (int)c->status.show_battery, (int)c->status.show_wifi,
           (int)c->status.wifi_fail_slash);

    printf("colors : bg 0x%06X, time 0x%06X / 0x%06X\n",
           (unsigned)c->colors.background, (unsigned)c->colors.time_am,
           (unsigned)c->colors.time_pm);
    printf("colors : battery 0x%06X / 0x%06X / 0x%06X\n",
           (unsigned)c->colors.battery_green,
           (unsigned)c->colors.battery_orange,
           (unsigned)c->colors.battery_red);
    printf("colors : wifi 0x%06X / 0x%06X / 0x%06X, tap 0x%06X\n",
           (unsigned)c->colors.wifi_idle, (unsigned)c->colors.wifi_ok,
           (unsigned)c->colors.wifi_fail, (unsigned)c->colors.tap_border);

    printf("clock  : tz %s, auto %d\n", c->clock.timezone,
           (int)c->clock.auto_timezone);
    printf("clock  : sync %u/day (0 = use sync_hours %u), wifi always %d\n",
           (unsigned)c->clock.sync_per_day, (unsigned)c->clock.sync_hours,
           (int)c->clock.wifi_always_on);
    printf("clock  : ntp %s / %s\n", c->clock.ntp1, c->clock.ntp2);
    printf("clock  : geo %s, manual '%s'\n", c->clock.geo_url,
           c->clock.manual_time);

    printf("speech : enabled %d, volume %u%%, day %d, gap %u ms\n",
           (int)c->announce.enabled, (unsigned)c->announce.volume,
           (int)c->announce.announce_day, (unsigned)c->announce.clip_gap_ms);
    printf("speech : double tap %u ms, hold %u ms, move %u px, cooldown %u ms\n",
           (unsigned)c->announce.double_tap_ms,
           (unsigned)c->announce.max_hold_ms,
           (unsigned)c->announce.max_move_px,
           (unsigned)c->announce.cooldown_ms);
    printf("speech : clips in %s\n", c->announce.voice_dir);

    printf("wifi   : %u network(s)\n", (unsigned)c->wifi_count);
    for (uint8_t i = 0; i < c->wifi_count && i < MOSU_CFG_WIFI_MAX; i++) {
        printf("       %u: %s\n", (unsigned)(i + 1), c->wifi[i].ssid);
    }
}

static void say_now(void)
{
    struct tm now;
    mosu_time_now(&now);

    if (!mosu_time_valid()) {
        printf("clock not set, cannot speak\n");
        return;
    }

    mosu_phrase_t phrase;
    mosu_phrase_build((uint8_t)now.tm_hour, (uint8_t)now.tm_min,
                      mosu_weekday_from_tm(now.tm_wday),
                      s_cfg->announce.announce_day, &phrase);
    mosu_audio_say(&phrase);
    printf("speaking %u clip(s)\n", (unsigned)phrase.count);
}

static mosu_ui_test_t parse_pattern(const char *name)
{
    if (!strcmp(name, "colors")) {
        return MOSU_UI_TEST_COLORS;
    }
    if (!strcmp(name, "grid")) {
        return MOSU_UI_TEST_GRID;
    }
    if (!strcmp(name, "corners")) {
        return MOSU_UI_TEST_CORNERS;
    }
    if (!strcmp(name, "off")) {
        return MOSU_UI_TEST_NONE;
    }
    return (mosu_ui_test_t)-1;
}

static void test_screen(const char *name)
{
    mosu_ui_test_t pattern = parse_pattern(name);
    if ((int)pattern < 0) {
        printf("usage: test screen [colors|grid|corners|off]\n");
        return;
    }

    printf("test screen: %s\n", name);
    /* app_main owns the panel state: going through the hook keeps the pattern
     * from being drawn on a sleeping display. */
    if (s_hooks.test_pattern) {
        s_hooks.test_pattern((int)pattern);
    } else {
        mosu_ui_test_pattern(pattern);
    }
}

static void handle_line(char *line)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        printf("commands:\n");
        printf("  status time sync say settime bright vol tap reboot\n");
        printf("  i2c pmu rtc imu pm heap tasks display config\n");
        printf("  test screen [colors|grid|corners|off]\n");
        printf("  test touch | test audio | sd remount\n");
        printf("  wifi <ssid> <password> | scan | presence | clear | status\n");
        printf("  screenshot [path] | power screen on|off | power wifi |\n");
        printf("  power idle\n");
    } else if (!strcmp(line, "status")) {
        print_status();
    } else if (!strcmp(line, "time")) {
        struct tm now;
        mosu_time_now(&now);
        printf("%04d-%02d-%02d %02d:%02d:%02d, tz %s, valid %d\n",
               now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour,
               now.tm_min, now.tm_sec, mosu_time_timezone(),
               (int)mosu_time_valid());
    } else if (!strcmp(line, "sync")) {
        mosu_net_request_sync();
        printf("sync requested\n");
    } else if (!strcmp(line, "say")) {
        say_now();
    } else if (!strncmp(line, "settime ", 8)) {
        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        if (sscanf(line + 8, "%d-%d-%d %d:%d", &year, &month, &day, &hour,
                   &minute) != 5 ||
            year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59) {
            printf("usage: settime YYYY-MM-DD HH:MM (local time)\n");
            return;
        }

        struct tm local = {
            .tm_year = year - 1900,
            .tm_mon = month - 1,
            .tm_mday = day,
            .tm_hour = hour,
            .tm_min = minute,
            .tm_isdst = -1,
        };
        time_t epoch = mktime(&local);
        if (epoch <= 0 || mosu_time_set_epoch_manual(epoch) != ESP_OK) {
            printf("clock could not be set\n");
            return;
        }
        printf("clock set to %04d-%02d-%02d %02d:%02d local\n", year, month,
               day, hour, minute);
    } else if (!strcmp(line, "tap")) {
        mosu_ui_flash_tap();
    } else if (!strncmp(line, "bright ", 7)) {
        int value = atoi(line + 7);
        if (value < 1 || value > 100) {
            printf("brightness must be 1..100\n");
            return;
        }
        s_cfg->display.brightness = (uint8_t)value;
        mosu_board_brightness_set((uint8_t)value);
        printf("brightness %d%% (runtime only)\n", value);
    } else if (!strncmp(line, "vol ", 4)) {
        int value = atoi(line + 4);
        if (value < 0 || value > 100) {
            printf("volume must be 0..100\n");
            return;
        }
        s_cfg->announce.volume = (uint8_t)value;
        mosu_audio_set_volume((uint8_t)value);
        printf("volume %d%% (runtime only)\n", value);
    } else if (!strcmp(line, "i2c")) {
        esp_err_t err = mosu_board_i2c_scan();
        if (err != ESP_OK) {
            printf("i2c : scan error %s\n", esp_err_to_name(err));
        }
    } else if (!strcmp(line, "pmu")) {
        mosu_board_pmu_dump();
    } else if (!strcmp(line, "rtc")) {
        mosu_board_rtc_dump();
    } else if (!strcmp(line, "imu")) {
        mosu_board_imu_dump();
    } else if (!strcmp(line, "pm")) {
        mosu_board_pm_dump();
    } else if (!strcmp(line, "heap")) {
        mosu_board_heap_dump();
    } else if (!strcmp(line, "tasks")) {
        mosu_board_tasks_dump();
    } else if (!strcmp(line, "display")) {
        mosu_board_display_info_t info;
        mosu_board_display_info(&info);
        printf("display: %ldx%ld, %s, panel brightness %u%%, configured %u%%\n",
               (long)info.width, (long)info.height, info.on ? "on" : "off",
               (unsigned)info.brightness,
               s_cfg ? (unsigned)s_cfg->display.brightness : 0u);
        printf("  buffer: %lu px, double %d, dma %d, spiram %d\n",
               (unsigned long)info.draw_buffer_pixels, info.double_buffer,
               info.buffer_dma, info.buffer_spiram);
        printf("  panel: %s, touch: %s\n", info.panel, info.touch);
    } else if (!strcmp(line, "config")) {
        print_config();
    } else if (!strcmp(line, "wifi")) {
        printf("usage: wifi <ssid> <password> | wifi clear | wifi status\n");
    } else if (!strncmp(line, "wifi ", 5)) {
        const char *arg = line + 5;
        if (!strcmp(arg, "scan")) {
            esp_err_t err = mosu_net_scan();
            if (err != ESP_OK) {
                printf("wifi scan: %s\n", esp_err_to_name(err));
            }
        } else if (!strcmp(arg, "presence")) {
            printf("wifi: configured network %s\n",
                   mosu_net_home_ap_visible() ? "visible" : "not visible");
        } else if (!strcmp(arg, "status")) {
            printf("wifi: %u network(s)\n", (unsigned)s_cfg->wifi_count);
            for (uint8_t i = 0; i < s_cfg->wifi_count; i++) {
                printf("      %u: %s\n", (unsigned)(i + 1), s_cfg->wifi[i].ssid);
            }
        } else if (!strcmp(arg, "clear")) {
            memset(s_cfg->wifi, 0, sizeof(s_cfg->wifi));
            s_cfg->wifi_count = 0;
            mosu_net_clear_networks();
            esp_err_t err = mosu_config_store_save(s_cfg);
            printf("wifi: networks cleared, saved %s\n",
                   err == ESP_OK ? "to NVS" : "FAILED");
        } else {
            /* The SSID ends at the first space; everything after it is the
             * password, so a passphrase containing spaces still works. */
            char work[96];
            snprintf(work, sizeof(work), "%s", arg);
            char *space = strchr(work, ' ');
            if (!space) {
                printf("usage: wifi <ssid> <password>\n");
                return;
            }
            *space = '\0';
            const char *ssid = work;
            const char *pass = space + 1;
            if (*ssid == '\0') {
                printf("usage: wifi <ssid> <password>\n");
                return;
            }

            esp_err_t err = mosu_net_set_network(ssid, pass);
            if (err != ESP_OK) {
                printf("wifi: rejected (%s)\n", esp_err_to_name(err));
                return;
            }

            memset(s_cfg->wifi, 0, sizeof(s_cfg->wifi));
            size_t ssid_len = strlen(ssid);
            size_t pass_len = strlen(pass);
            if (ssid_len < sizeof(s_cfg->wifi[0].ssid) &&
                pass_len < sizeof(s_cfg->wifi[0].pass)) {
                memcpy(s_cfg->wifi[0].ssid, ssid, ssid_len + 1);
                memcpy(s_cfg->wifi[0].pass, pass, pass_len + 1);
            }
            s_cfg->wifi_count = 1;

            err = mosu_config_store_save(s_cfg);
            printf("wifi: network 1 = '%s', saved %s, sync requested\n", ssid,
                   err == ESP_OK ? "to NVS" : "FAILED");
            mosu_net_request_sync();
        }
    } else if (!strcmp(line, "test screen")) {
        printf("usage: test screen [colors|grid|corners|off]\n");
    } else if (!strncmp(line, "test screen ", 12)) {
        test_screen(line + 12);
    } else if (!strcmp(line, "test touch")) {
        s_touch_test = !s_touch_test;
        printf("touch test %s\n",
               s_touch_test ? "on: touch edges will be logged"
                            : "off");
    } else if (!strcmp(line, "test audio")) {
        esp_err_t err = mosu_board_audio_test();
        printf("audio test: %s\n", esp_err_to_name(err));
    } else if (!strcmp(line, "sd remount")) {
        esp_err_t err = mosu_board_sdcard_remount();
        printf("sd remount: %s, %s\n", esp_err_to_name(err),
               mosu_board_sdcard_ready() ? "mounted" : "not mounted");
    } else if (!strcmp(line, "screenshot") ||
               !strncmp(line, "screenshot ", 11)) {
        const char *path = "/sdcard/shot.bmp";
        if (!strncmp(line, "screenshot ", 11) && line[11] != '\0') {
            path = line + 11;
        }

        /* The panel may be off.  The hook only queues a request; give the
         * app task time to switch the face on and repaint before capturing. */
        if (s_hooks.wake) {
            s_hooks.wake();
            vTaskDelay(pdMS_TO_TICKS(300));
        } else if (s_hooks.screen_on) {
            s_hooks.screen_on();
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        esp_err_t err = mosu_ui_snapshot(path);
        printf("screenshot %s: %s\n", path, esp_err_to_name(err));
    } else if (!strcmp(line, "power screen on")) {
        if (!s_hooks.screen_on) {
            printf("no screen on hook\n");
            return;
        }
        s_hooks.screen_on();
        printf("screen on\n");
    } else if (!strcmp(line, "power screen off")) {
        if (!s_hooks.screen_off) {
            printf("no screen off hook\n");
            return;
        }
        s_hooks.screen_off();
        printf("screen off\n");
    } else if (!strcmp(line, "power wifi")) {
        if (!s_hooks.power_wifi) {
            printf("no wifi power hook\n");
            return;
        }
        s_hooks.power_wifi();
        printf("wifi power requested\n");
    } else if (!strcmp(line, "power idle")) {
        if (!s_hooks.power_idle) {
            printf("no idle power hook\n");
            return;
        }
        s_hooks.power_idle();
        printf("idle power requested\n");
    } else if (!strcmp(line, "reboot")) {
        printf("rebooting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    } else {
        printf("unknown command: %s\n", line);
    }
}

static void diag_task(void *arg)
{
    (void)arg;

    char line[96];
    size_t len = 0;

    printf("MosuTime console ready, type 'help'\n");

    for (;;) {
        char ch = 0;
        if (read(STDIN_FILENO, &ch, 1) != 1) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            handle_line(line);
            len = 0;
        } else if (ch == 0x7F || ch == 0x08) {
            if (len > 0) {
                len--;
            }
        } else if (len + 1 < sizeof(line)) {
            line[len++] = ch;
        }
    }
}

esp_err_t mosu_diag_start(mosu_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;

    if (xTaskCreate(diag_task, "mosu_diag", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "console started");
    return ESP_OK;
}

void mosu_diag_set_hooks(const mosu_diag_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }
}

bool mosu_diag_touch_test_active(void)
{
    return s_touch_test;
}
