/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * mosu_config is pure C: it can be compiled and tested on a PC.  The NVS
 * cache lives in a separate file so that this parser never needs ESP-IDF.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOSU_CFG_WIFI_MAX 8
#define MOSU_CFG_SSID_MAX 33
#define MOSU_CFG_PASS_MAX 65
#define MOSU_CFG_TZ_MAX 64
#define MOSU_CFG_PATH_MAX 96
#define MOSU_CFG_URL_MAX 128
#define MOSU_CFG_NAME_MAX 32

typedef struct {
    char ssid[MOSU_CFG_SSID_MAX];
    char pass[MOSU_CFG_PASS_MAX];
} mosu_wifi_cfg_t;

typedef struct {
    char name[MOSU_CFG_NAME_MAX];
    uint8_t log_level; /* 0 none .. 5 verbose */
    bool stats;        /* background power accounting and `stats` */
    bool diagnostics;  /* SD/NVS power logs and PM/task dumps */
} mosu_device_cfg_t;

typedef struct {
    uint8_t brightness;      /* 1..100, constant, applied as panel 0x51 */
    uint32_t screen_on_ms;   /* how long the screen stays lit after a wake */
    bool wake_on_touch;
    bool wake_on_motion;
    uint16_t motion_angle_deg;     /* wrist raise sensitivity */
    uint16_t motion_refractory_ms;
    uint8_t motion_poll_hz;        /* accelerometer poll rate while asleep */
    uint16_t pixel_shift_sec;      /* 0 = disabled, AMOLED burn-in guard */
    uint16_t tap_feedback_ms;      /* 0 = no border flash */
    bool panel_sleep;              /* panel 0x10 while off (loses GRAM) */
} mosu_display_cfg_t;

typedef struct {
    uint32_t background;
    uint32_t time_am;
    uint32_t time_pm;
    uint32_t battery_green;
    uint32_t battery_orange;
    uint32_t battery_red;
    uint32_t wifi_idle;
    uint32_t wifi_ok;
    uint32_t wifi_fail;
    uint32_t tap_border;
} mosu_colors_cfg_t;

typedef struct {
    uint8_t battery_green_min;
    uint8_t battery_orange_min;
    bool show_battery;
    bool show_wifi;
    bool wifi_fail_slash;
} mosu_status_cfg_t;

typedef struct {
    char timezone[MOSU_CFG_TZ_MAX]; /* POSIX TZ, always a valid fallback */
    bool auto_timezone;             /* ask the network for the zone */
    char geo_url[MOSU_CFG_URL_MAX]; /* plain-text, two-line name+offset, JSON */
    char ntp1[MOSU_CFG_URL_MAX];
    char ntp2[MOSU_CFG_URL_MAX];
    uint16_t sync_hours;
    /* 1..24 = sync this many times per day; 0 = use sync_hours. */
    uint8_t sync_per_day;
    bool wifi_always_on; /* false = sync_only (best battery life) */
    char manual_time[24]; /* "YYYY-MM-DD HH:MM" fallback when all else fails */
} mosu_clock_cfg_t;

typedef struct {
    bool enabled;
    uint16_t double_tap_ms;
    uint16_t max_hold_ms;
    uint16_t max_move_px;
    uint16_t cooldown_ms;
    uint8_t volume; /* 0..100, applied with esp_codec_dev_set_out_vol() */
    bool announce_day;
    char voice_dir[MOSU_CFG_PATH_MAX];
    uint16_t clip_gap_ms;
} mosu_announce_cfg_t;

typedef struct {
    mosu_device_cfg_t device;
    mosu_display_cfg_t display;
    mosu_colors_cfg_t colors;
    mosu_status_cfg_t status;
    mosu_clock_cfg_t clock;
    mosu_announce_cfg_t announce;

    mosu_wifi_cfg_t wifi[MOSU_CFG_WIFI_MAX];
    uint8_t wifi_count;
} mosu_config_t;

typedef struct {
    int lines;
    int applied;
    int warnings;
    int errors;
    char message[128]; /* first error or warning, human readable */
} mosu_config_report_t;

void mosu_config_defaults(mosu_config_t *cfg);

/*
 * Parse an INI document.  Unknown keys and malformed values are reported but
 * never abort the load: every field keeps its previous (default) value.
 * Returns 0 on success, -1 on a NULL argument.
 */
int mosu_config_parse(mosu_config_t *cfg, const char *text, size_t len,
                      mosu_config_report_t *report);

/* Reads the whole file.  Returns 0 on success, -1 when it cannot be opened. */
int mosu_config_load(mosu_config_t *cfg, const char *path,
                     mosu_config_report_t *report);

/* Clamp every field into a safe range and fix inconsistent combinations. */
void mosu_config_sanitize(mosu_config_t *cfg);

/* True when a timezone string is safe to hand to setenv("TZ", ...). */
bool mosu_config_timezone_ok(const char *tz);

#ifdef __cplusplus
}
#endif
