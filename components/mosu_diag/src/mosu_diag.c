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
#include "mosu_core/mosu_core.h"
#include "mosu_diag.h"
#include "mosu_net.h"
#include "mosu_time.h"
#include "mosu_ui.h"

static const char *TAG = "mosu_diag";
static mosu_config_t *s_cfg;

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

static void handle_line(char *line)
{
    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    if (!strcmp(line, "help") || !strcmp(line, "?")) {
        printf("commands: help status time sync say settime bright vol tap reboot\n");
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
