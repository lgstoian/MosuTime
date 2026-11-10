/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The RTC is the source of truth until the network says otherwise.  Everything
 * inside the firmware is UTC; the timezone only affects what is displayed and
 * spoken.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_system.h"

#include "mosu_board.h"
#include "mosu_config_store.h"
#include "mosu_time.h"

static const char *TAG = "mosu_time";

static char s_tz_buf[2][MOSU_CFG_TZ_MAX];
static volatile char *s_tz = s_tz_buf[0];
static bool s_valid;
static bool s_have_sync;
static time_t s_last_sync;

typedef struct {
    const char *iana;
    const char *posix;
} tz_map_t;

/*
 * newlib (ESP-IDF) understands POSIX TZ strings, not the IANA tzdata names a
 * geolocation service returns.  This table covers Romania and the places a
 * family is likely to travel to; anything unknown keeps the configured zone.
 */
static const tz_map_t s_tz_map[] = {
    {"Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Sofia", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Kiev", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Riga", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Tallinn", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Europe/Dublin", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0/2"},
    {"Europe/Paris", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Brussels", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Prague", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Budapest", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Copenhagen", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Stockholm", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Belgrade", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Zagreb", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Sarajevo", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Skopje", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Podgorica", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Tirane", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Malta", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Europe/Istanbul", "TRT-3"},
    {"Europe/Moscow", "MSK-3"},
    {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Toronto", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"America/Vancouver", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"Asia/Dubai", "GST-4"},
    {"Asia/Bangkok", "ICT-7"},
    {"Asia/Singapore", "SGT-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"UTC", "UTC0"},
    {"Etc/UTC", "UTC0"},
};

const char *mosu_time_timezone(void)
{
    return (const char *)s_tz;
}

static const char *posix_for_zone(const char *tz)
{
    for (size_t i = 0; i < sizeof(s_tz_map) / sizeof(s_tz_map[0]); i++) {
        if (!strcmp(s_tz_map[i].iana, tz)) {
            return s_tz_map[i].posix;
        }
    }
    return NULL;
}

/*
 * newlib's timegm() is not exposed by the ESP-IDF headers, so convert a UTC
 * struct tm with the standard civil-from-days algorithm instead.
 */
static time_t utc_from_tm(const struct tm *t)
{
    int month = t->tm_mon + 1;
    int year = t->tm_year + 1900;
    year -= (month <= 2);

    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy =
        (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 +
                   t->tm_mday - 1);
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;

    int64_t seconds = days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 +
                      t->tm_sec;
    return (time_t)seconds;
}

static bool set_zone(const char *tz, bool persist){
    if (!mosu_config_timezone_ok(tz)) {
        return false;
    }

    const char *posix = posix_for_zone(tz);
    if (!posix) {
        posix = tz;
    }

    if (setenv("TZ", posix, 1) != 0) {
        return false;
    }
    tzset();

    /* Double buffer: the network task writes the zone while the console and
     * the UI may read it.  The pointer swap is atomic on the ESP32. */
    char *next = (s_tz == s_tz_buf[0]) ? s_tz_buf[1] : s_tz_buf[0];
    snprintf(next, sizeof(s_tz_buf[0]), "%s", tz);
    s_tz = next;
    ESP_LOGI(TAG, "timezone: %s (%s)", next, posix);

    if (persist) {
        mosu_config_store_save_timezone(tz);
    }
    return true;
}

bool mosu_time_apply_timezone(const char *tz)
{
    if (!tz || !*tz) {
        return false;
    }
    /* Do not override a good zone with an unmapped geolocation result. */
    if (!posix_for_zone(tz) && strchr(tz, '/')) {
        ESP_LOGW(TAG, "unknown IANA zone '%s', keeping %s", tz, s_tz);
        return false;
    }
    return set_zone(tz, true);
}

static bool apply_manual_time(const char *text)
{
    int year, month, day, hour, minute;
    if (sscanf(text, "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute) !=
        5) {
        return false;
    }
    if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
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
    if (epoch <= 0) {
        return false;
    }

    struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);
    s_valid = true;
    ESP_LOGW(TAG, "clock set from manual_time in config");
    return true;
}

esp_err_t mosu_time_init(const mosu_config_t *cfg)
{
    s_valid = false;
    s_have_sync = false;
    s_last_sync = 0;

    const char *configured = cfg ? cfg->clock.timezone : "UTC0";
    char stored[MOSU_CFG_TZ_MAX];

    bool auto_tz = cfg ? cfg->clock.auto_timezone : false;
    bool zone_ok = false;
    if (auto_tz && mosu_config_store_load_timezone(stored, sizeof(stored))) {
        zone_ok = set_zone(stored, false);
    }
    if (!zone_ok) {
        zone_ok = set_zone(configured, false);
    }
    if (!zone_ok) {
        /* Never leave newlib without a zone: localtime would silently return
         * UTC and the watch would show the wrong hour. */
        set_zone("UTC0", false);
    }

    struct tm utc;
    if (mosu_board_rtc_read(&utc) == ESP_OK && mosu_board_rtc_valid()) {
        time_t epoch = utc_from_tm(&utc);
        struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
        if (epoch > 0) {
            settimeofday(&tv, NULL);
            s_valid = true;
            ESP_LOGI(TAG, "clock restored from RTC");
        }
    }

    if (!s_valid && cfg && cfg->clock.manual_time[0]) {
        apply_manual_time(cfg->clock.manual_time);
    }

    if (!s_valid) {
        ESP_LOGW(TAG, "clock not set yet; waiting for network");
    }
    return ESP_OK;
}

void mosu_time_now(struct tm *out)
{
    if (!out) {
        return;
    }
    time_t now = time(NULL);
    localtime_r(&now, out);
}

bool mosu_time_valid(void)
{
    return s_valid;
}

esp_err_t mosu_time_set_epoch(time_t utc)
{
    if (utc <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv = {.tv_sec = utc, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    s_valid = true;
    s_have_sync = true;
    s_last_sync = utc;
    mosu_time_persist_rtc();
    ESP_LOGI(TAG, "clock synced");
    return ESP_OK;
}

esp_err_t mosu_time_set_epoch_manual(time_t utc)
{
    if (utc <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv = {.tv_sec = utc, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    s_valid = true;
    mosu_time_persist_rtc();
    ESP_LOGI(TAG, "clock set manually");
    return ESP_OK;
}

uint32_t mosu_time_sync_age_s(void)
{
    if (!s_have_sync) {
        return UINT32_MAX;
    }
    time_t now = time(NULL);
    if (now < s_last_sync) {
        return 0;
    }
    return (uint32_t)(now - s_last_sync);
}

esp_err_t mosu_time_persist_rtc(void)
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    return mosu_board_rtc_write(&utc);
}
