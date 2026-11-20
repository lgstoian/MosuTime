/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics over the console UART.  This is deliberately tiny: a handful of
 * commands that answer "is it synced, is the card there, does it speak".
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#include "mosu_audio.h"
#include "mosu_board.h"
#include "mosu_config_store.h"
#include "mosu_core/mosu_core.h"
#include "mosu_diag.h"
#include "mosu_net.h"
#include "mosu_tasks.h"
#include "mosu_time.h"
#include "mosu_ui.h"

static const char *TAG = "mosu_diag";
static mosu_config_t *s_cfg;

static mosu_diag_hooks_t s_hooks;
static volatile bool s_touch_test;
static bool s_sd_verbose;

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

/*
 * Read exactly len bytes from the console.  With USB Serial/JTAG the console
 * driver's ring buffer is used directly: the ISR drains the 64-byte hardware
 * FIFO, so a higher-priority task cannot make the reader lose a byte, and one
 * call returns a whole block.  Reading the driver also bypasses the VFS
 * CR-to-LF translation, which would corrupt a binary 0x0D.  The UART and
 * emulator builds fall back to the VFS.
 */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static bool s_usj_ready;
#endif

static bool read_exact(uint8_t *buffer, size_t len)
{
    size_t got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    while (got < len) {
        ssize_t n = -1;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        if (s_usj_ready) {
            n = usb_serial_jtag_read_bytes(buffer + got, len - got,
                                           pdMS_TO_TICKS(1000));
        } else
#endif
        {
            n = read(STDIN_FILENO, buffer + got, len - got);
        }
        if (n > 0) {
            got += (size_t)n;
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR) {
            return false;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

/*
 * Binary upload used by tools/push_sd.py.  The host sends a 4096-byte chunk,
 * waits for "RX <cumulative>" and only then sends the next one, so the USB
 * Serial/JTAG path cannot overrun and the ACK means the bytes reached the
 * file.  The optional /crc trailer is a little-endian CRC-32.
 */
static bool receive_file(const char *path, uint32_t size, bool verify_crc)
{
    if (!mosu_board_sdcard_ready()) {
        printf("receive: no SD card\n");
        return false;
    }
    if (size == 0 || size > 1024 * 1024) {
        printf("receive: size must be 1..1048576\n");
        return false;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        printf("receive: cannot create %s (%s)\n", path, strerror(errno));
        return false;
    }

    printf("\n=== RX READY ===\n");
    fflush(stdout);

    uint8_t buffer[4096];
    uint32_t done = 0;
    uint32_t crc = 0;

    while (done < size) {
        uint32_t want = size - done;
        if (want > sizeof(buffer)) {
            want = sizeof(buffer);
        }

        if (!read_exact(buffer, want)) {
            fclose(file);
            remove(path);
            printf("receive: input stalled at %u/%u bytes\n", (unsigned)done,
                   (unsigned)size);
            return false;
        }

        if (fwrite(buffer, 1, want, file) != want) {
            fclose(file);
            remove(path);
            printf("receive: write failed at %u/%u bytes\n", (unsigned)done,
                   (unsigned)size);
            return false;
        }
        crc = esp_rom_crc32_le(crc, buffer, want);
        done += want;
        printf("RX %u\n", (unsigned)done);
        fflush(stdout);
    }

    if (verify_crc) {
        uint8_t trailer[4];
        if (!read_exact(trailer, sizeof(trailer))) {
            fclose(file);
            remove(path);
            printf("receive: missing CRC trailer\n");
            return false;
        }
        uint32_t expected = (uint32_t)trailer[0] |
                            ((uint32_t)trailer[1] << 8) |
                            ((uint32_t)trailer[2] << 16) |
                            ((uint32_t)trailer[3] << 24);
        if (expected != crc) {
            fclose(file);
            remove(path);
            printf("receive: CRC mismatch (got 0x%08X, want 0x%08X)\n",
                   (unsigned)crc, (unsigned)expected);
            return false;
        }
    }

    if (fclose(file) != 0) {
        remove(path);
        printf("receive: close failed (%s)\n", strerror(errno));
        return false;
    }

    printf("=== RX DONE ===\n");
    fflush(stdout);
    return true;
}

static void rx_bench(uint32_t size)
{
    uint8_t buffer[4096];
    uint32_t done = 0;
    const int64_t start = esp_timer_get_time();

    printf("\n=== RX READY ===\n");
    fflush(stdout);

    while (done < size) {
        uint32_t want = size - done;
        if (want > sizeof(buffer)) {
            want = sizeof(buffer);
        }
        if (!read_exact(buffer, want)) {
            printf("rx bench: stalled at %u\n", (unsigned)done);
            break;
        }
        done += want;
        printf("RX %u\n", (unsigned)done);
        fflush(stdout);
    }

    const int64_t elapsed_us = esp_timer_get_time() - start;
    const int64_t ms = elapsed_us / 1000;
    printf("rx bench: %u bytes in %lld ms (%.1f KiB/s)\n", (unsigned)done,
           (long long)ms,
           ms > 0 ? (double)done / 1024.0 * 1000.0 / (double)ms : 0.0);
    printf("=== RX DONE ===\n");
    fflush(stdout);
}

static void list_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        printf("ls %s: %s\n", path, strerror(errno));
        return;
    }

    unsigned entries = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        printf("  %s%s\n", ent->d_name,
               ent->d_type == DT_DIR ? "/" : "");
        entries++;
    }
    closedir(dir);
    printf("ls %s: %u entries\n", path, entries);
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
        printf("  status stats [reset] time sync say settime bright vol tap reboot version\n");
        printf("  i2c pmu rtc imu pm heap tasks display config mem\n");
        printf("  test screen [colors|grid|corners|off] | scene clock|blank\n");
        printf("  test touch | test audio | sd remount | sd verbose | sd dump\n");
        printf("  sd format yes | receive <path> <size> [/crc] | md <path>\n");
        printf("  ls [path] | rm <path> | rx bench <bytes>\n");
        printf("  wifi <ssid> <password> | scan | presence | clear | status\n");
        printf("  screenshot [path] | power screen on|off | power wifi |\n");
        printf("  power idle | poweroff | nvs erase\n");
    } else if (!strncmp(line, "receive ", 8)) {
        char path[128];
        unsigned long size = 0;
        char option[8] = {0};
        int n = sscanf(line + 8, "%127s %lu %7s", path, &size, option);
        if (n < 2) {
            printf("usage: receive <path> <size> [/crc]\n");
            return;
        }
        bool verify_crc = n >= 3 && !strcmp(option, "/crc");
        receive_file(path, (uint32_t)size, verify_crc);
    } else if (!strncmp(line, "rx bench ", 9)) {
        uint32_t size = (uint32_t)strtoul(line + 9, NULL, 10);
        if (size == 0 || size > 4 * 1024 * 1024) {
            printf("usage: rx bench <bytes> (1..4194304)\n");
            return;
        }
        rx_bench(size);
    } else if (!strncmp(line, "md ", 3) || !strncmp(line, "mkdir ", 6)) {
        const char *path =
            !strncmp(line, "mkdir ", 6) ? line + 6 : line + 3;
        errno = 0;
        if (mkdir(path, 0777) == 0) {
            printf("mkdir %s\n", path);
        } else if (errno == EEXIST) {
            printf("mkdir %s: exists\n", path);
        } else {
            printf("mkdir %s: %s\n", path, strerror(errno));
        }
    } else if (!strncmp(line, "rm ", 3)) {
        if (remove(line + 3) == 0) {
            printf("rm %s\n", line + 3);
        } else {
            printf("rm %s: %s\n", line + 3, strerror(errno));
        }
    } else if (!strcmp(line, "ls")) {
        list_dir("/sdcard");
    } else if (!strncmp(line, "ls ", 3)) {
        list_dir(line + 3);
    } else if (!strcmp(line, "version")) {
        const esp_app_desc_t *app = esp_app_get_description();
        esp_chip_info_t chip;
        esp_chip_info(&chip);
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        printf("app    : %s %s, built %s %s\n", app->project_name,
               app->version, app->date, app->time);
        printf("idf    : %s\n", app->idf_ver);
        printf("mac    : %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1],
               mac[2], mac[3], mac[4], mac[5]);
        printf("chip   : ESP32-S3 rev %d.%d, %d core(s)\n",
               chip.revision / 100, chip.revision % 100, chip.cores);
    } else if (!strcmp(line, "stats")) {
        mosu_diag_stats_t stats;
        if (!s_hooks.stats) {
            printf("stats: no hook\n");
            return;
        }
        s_hooks.stats(&stats);
        const uint32_t minutes = stats.uptime_s / 60u;
        printf("stats : up %uh%02um, screen on %um (%u%%), %u wakes, %u boots\n",
               (unsigned)(minutes / 60u), (unsigned)(minutes % 60u),
               (unsigned)(stats.screen_on_s / 60u),
               stats.uptime_s ? (unsigned)(stats.screen_on_s * 100u /
                                           stats.uptime_s)
                              : 0u,
               (unsigned)stats.wakes, (unsigned)stats.boots);
        printf("power : %u mV, %u%%, charging %d, boot %u%%\n",
               (unsigned)stats.battery_mv, (unsigned)stats.battery_pct,
               (int)stats.charging, (unsigned)stats.boot_pct);
        printf("auto  : screen %u s, raise %u deg/%u ms, still %u ms, "
               "face %u/1000\n",
               (unsigned)(stats.screen_ms / 1000u), (unsigned)stats.angle_deg,
               (unsigned)stats.hold_ms, (unsigned)stats.still_ms,
               (unsigned)stats.face_milli);
        if (!stats.charging && stats.battery_present && stats.uptime_s > 600 &&
            stats.boot_pct > stats.battery_pct) {
            const unsigned used = stats.boot_pct - stats.battery_pct;
            const double hours = stats.uptime_s / 3600.0;
            const double ma = used * 4.0 / hours;
            printf("drain : %u%% in %.1f h, about %.1f mA average "
                   "(400 mAh cell, estimate)\n",
                   used, hours, ma);
        }
    } else if (!strcmp(line, "stats reset")) {
        if (!s_hooks.stats_reset) {
            printf("stats: no hook\n");
            return;
        }
        s_hooks.stats_reset();
        printf("stats: baseline reset at the current battery percentage\n");
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
    } else if (!strncmp(line, "scene ", 6)) {
        const char *name = line + 6;
        if (!strcmp(name, "clock")) {
            mosu_ui_set_scene(MOSU_UI_SCENE_CLOCK);
            printf("scene: clock\n");
        } else if (!strcmp(name, "blank")) {
            mosu_ui_set_scene(MOSU_UI_SCENE_BLANK);
            printf("scene: blank\n");
        } else {
            printf("usage: scene clock|blank\n");
        }
    } else if (!strcmp(line, "mem")) {
        mosu_tasks_report();
        printf("mem   : reset reason %d\n", (int)esp_reset_reason());
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
    } else if (!strcmp(line, "sd verbose")) {
        s_sd_verbose = !s_sd_verbose;
        mosu_board_sdcard_set_verbose(s_sd_verbose);
        printf("sd verbose %s\n", s_sd_verbose ? "on" : "off");
    } else if (!strcmp(line, "sd dump")) {
        esp_err_t err = mosu_board_sdcard_dump_boot();
        printf("sd dump: %s\n", esp_err_to_name(err));
    } else if (!strcmp(line, "sd format")) {
        printf("sd format: this erases the card; use 'sd format yes'\n");
    } else if (!strcmp(line, "sd format yes")) {
        printf("sd format: formatting, do not remove the card...\n");
        fflush(stdout);
        esp_err_t err = mosu_board_sdcard_format();
        printf("sd format: %s, %s\n", esp_err_to_name(err),
               mosu_board_sdcard_ready() ? "mounted" : "not mounted");
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
    } else if (!strcmp(line, "nvs erase")) {
        printf("nvs erase: wiping the cached config and Wi-Fi calibration...\n");
        fflush(stdout);
        esp_err_t err = nvs_flash_erase();
        printf("nvs erase: %s, rebooting\n", esp_err_to_name(err));
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (!strcmp(line, "poweroff") || !strcmp(line, "shutdown")) {
        printf("poweroff: saving the clock and cutting the rails\n");
        fflush(stdout);
        mosu_time_persist_rtc();
        if (s_hooks.screen_off) {
            s_hooks.screen_off();
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_err_t err = mosu_board_pmu_power_off();
        printf("poweroff: %s (VBUS keeps the board alive)\n",
               esp_err_to_name(err));
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

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* The bare console reads the 64-byte hardware FIFO a byte at a time; a
     * higher-priority task in the wrong moment can make it lose a byte and
     * stall a binary upload.  The USJ driver drains the FIFO in its ISR into
     * a ring buffer, and the same VFS then reads whole blocks from there. */
    usb_serial_jtag_driver_config_t usj_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 8192,
    };
    esp_err_t usj_err = usb_serial_jtag_driver_install(&usj_cfg);
    if (usj_err == ESP_OK || usj_err == ESP_ERR_INVALID_STATE) {
        usb_serial_jtag_vfs_use_driver();
        s_usj_ready = true;
    } else {
        ESP_LOGW(TAG, "USB console buffering: %s", esp_err_to_name(usj_err));
    }
#endif

    if (mosu_task_start(&(mosu_task_cfg_t){
                             .name = "mosu_diag",
                             .fn = diag_task,
                             .stack = 8192,
                             .priority = 2,
                             .core = tskNO_AFFINITY,
                         },
                         NULL) != ESP_OK) {
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
