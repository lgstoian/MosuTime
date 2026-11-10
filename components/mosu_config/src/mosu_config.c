/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "mosu_config.h"

#define LINE_MAX_LEN 256

typedef enum {
    SEC_NONE = 0,
    SEC_DEVICE,
    SEC_DISPLAY,
    SEC_COLORS,
    SEC_STATUS,
    SEC_CLOCK,
    SEC_WIFI,
    SEC_ANNOUNCE,
} section_t;

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return s;
}

/*
 * Remove "# ..." / "; ..." comments.  A marker only starts a comment when it
 * is at the start of the line, or preceded and followed by whitespace.  That
 * keeps colour values such as "#FFFFFF" intact while still allowing a trailing
 * "  # note" after a value.  Comments are therefore written " # like this".
 */
static void strip_comment(char *s)
{
    bool in_quote = false;
    char quote = '\0';

    for (char *p = s; *p; p++) {
        if (in_quote) {
            if (*p == quote) {
                in_quote = false;
            }
            continue;
        }
        if (*p == '"' || *p == '\'') {
            in_quote = true;
            quote = *p;
            continue;
        }
        if (*p == '#' || *p == ';') {
            bool at_start = (p == s);
            bool spaced = p > s && isspace((unsigned char)p[-1]) &&
                          (p[1] == '\0' || isspace((unsigned char)p[1]));
            if (at_start || spaced) {
                *p = '\0';
                return;
            }
        }
    }
}

static void unquote(char *s)
{
    size_t len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') ||
                     (s[0] == '\'' && s[len - 1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static bool parse_long(const char *text, long min, long max, long *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 0);
    if (end == text) {
        return false;
    }
    while (*end && isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    if (value < min) {
        value = min;
    }
    if (value > max) {
        value = max;
    }
    *out = value;
    return true;
}

static bool parse_bool(const char *text, bool *out)
{
    if (!strcasecmp(text, "true") || !strcasecmp(text, "yes") ||
        !strcasecmp(text, "on") || !strcmp(text, "1")) {
        *out = true;
        return true;
    }
    if (!strcasecmp(text, "false") || !strcasecmp(text, "no") ||
        !strcasecmp(text, "off") || !strcmp(text, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_color(const char *text, uint32_t *out)
{
    if (*text == '#') {
        text++;
    }
    size_t len = strlen(text);
    if (len != 3 && len != 6) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }

    unsigned long value = strtoul(text, NULL, 16);
    if (len == 3) {
        unsigned r = (unsigned)((value >> 8) & 0xF);
        unsigned g = (unsigned)((value >> 4) & 0xF);
        unsigned b = (unsigned)(value & 0xF);
        value = (r * 17u << 16) | (g * 17u << 8) | (b * 17u);
    }
    *out = (uint32_t)value;
    return true;
}

static void report_error(mosu_config_report_t *report, const char *fmt,
                         const char *key)
{
    if (!report) {
        return;
    }
    if (report->errors == 0) {
        snprintf(report->message, sizeof(report->message), fmt, key);
    }
    report->errors++;
}

static void report_warn(mosu_config_report_t *report, const char *fmt,
                        const char *key)
{
    if (!report) {
        return;
    }
    if (report->errors == 0 && report->warnings == 0) {
        snprintf(report->message, sizeof(report->message), fmt, key);
    }
    report->warnings++;
}

void mosu_config_defaults(mosu_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    copy_str(cfg->device.name, sizeof(cfg->device.name), "MosuTime");
    cfg->device.log_level = 3; /* info */

    cfg->display.brightness = 55;
    cfg->display.screen_on_ms = 15000;
    cfg->display.wake_on_touch = true;
    cfg->display.wake_on_motion = true;
    cfg->display.motion_angle_deg = 35;
    cfg->display.motion_refractory_ms = 2500;
    cfg->display.motion_poll_hz = 8;
    cfg->display.pixel_shift_sec = 180;
    cfg->display.tap_feedback_ms = 250;

    cfg->colors.background = 0x000000;
    cfg->colors.time_am = 0xFFFFFF;
    cfg->colors.time_pm = 0xFFC98A;
    cfg->colors.battery_green = 0x00E676;
    cfg->colors.battery_orange = 0xFFAB00;
    cfg->colors.battery_red = 0xFF1744;
    cfg->colors.wifi_idle = 0x8A8A8A;
    cfg->colors.wifi_ok = 0x00E676;
    cfg->colors.wifi_fail = 0xFF1744;
    cfg->colors.tap_border = 0xFFFFFF;

    cfg->status.battery_green_min = 40;
    cfg->status.battery_orange_min = 15;
    cfg->status.show_battery = true;
    cfg->status.show_wifi = true;
    cfg->status.wifi_fail_slash = true;

    copy_str(cfg->clock.timezone, sizeof(cfg->clock.timezone),
             "EET-2EEST,M3.5.0/3,M10.5.0/4");
    cfg->clock.auto_timezone = true;
    copy_str(cfg->clock.geo_url, sizeof(cfg->clock.geo_url),
             "https://ipapi.co/timezone/");
    copy_str(cfg->clock.ntp1, sizeof(cfg->clock.ntp1), "pool.ntp.org");
    copy_str(cfg->clock.ntp2, sizeof(cfg->clock.ntp2), "time.google.com");
    cfg->clock.sync_hours = 6;
    cfg->clock.wifi_always_on = false;
    cfg->clock.manual_time[0] = '\0';

    cfg->announce.enabled = true;
    cfg->announce.double_tap_ms = 650;
    cfg->announce.max_hold_ms = 400;
    cfg->announce.max_move_px = 40;
    cfg->announce.cooldown_ms = 2000;
    cfg->announce.volume = 85;
    cfg->announce.announce_day = true;
    copy_str(cfg->announce.voice_dir, sizeof(cfg->announce.voice_dir),
             "/sdcard/voice");
    cfg->announce.clip_gap_ms = 60;
}

static section_t section_from(const char *name)
{
    if (!strcasecmp(name, "device")) {
        return SEC_DEVICE;
    }
    if (!strcasecmp(name, "display")) {
        return SEC_DISPLAY;
    }
    if (!strcasecmp(name, "colors") || !strcasecmp(name, "colours")) {
        return SEC_COLORS;
    }
    if (!strcasecmp(name, "status")) {
        return SEC_STATUS;
    }
    if (!strcasecmp(name, "clock")) {
        return SEC_CLOCK;
    }
    if (!strcasecmp(name, "wifi")) {
        return SEC_WIFI;
    }
    if (!strcasecmp(name, "announce")) {
        return SEC_ANNOUNCE;
    }
    return SEC_NONE;
}

static bool set_int(const char *value, long min, long max, int *out)
{
    long parsed = 0;
    if (!parse_long(value, min, max, &parsed)) {
        return false;
    }
    *out = (int)parsed;
    return true;
}

/*
 * Copy a network field, but say so when the value does not fit.  Silently
 * cutting an SSID produces a watch that simply refuses to connect, which is
 * much harder to diagnose than one warning in the boot log.
 */
static void copy_net_str(char *dst, size_t dst_len, const char *src,
                         mosu_config_report_t *report, bool password,
                         const char *key)
{
    if (strlen(src) >= dst_len) {
        report_warn(report,
                    password ? "password too long for %s"
                             : "SSID too long for %s",
                    key);
    }
    copy_str(dst, dst_len, src);
}

static void parse_wifi_key(mosu_config_t *cfg, const char *key,
                           const char *value, mosu_config_report_t *report)
{
    if (strncasecmp(key, "network", 7) != 0) {
        return;
    }

    const char *p = key + 7;
    int index = 0;
    while (*p >= '0' && *p <= '9') {
        /* Fuzzed input can contain a thousand digits; stop accumulating
         * before the signed int overflows.  Anything past the last valid
         * network is rejected below anyway. */
        if (index <= 1000) {
            index = index * 10 + (*p - '0');
        }
        p++;
    }
    if (*p != '_' || index < 1 || index > MOSU_CFG_WIFI_MAX) {
        return;
    }
    p++;

    mosu_wifi_cfg_t *net = &cfg->wifi[index - 1];
    if (!strcasecmp(p, "ssid")) {
        copy_net_str(net->ssid, sizeof(net->ssid), value, report, false, key);
    } else if (!strcasecmp(p, "pass") || !strcasecmp(p, "password")) {
        copy_net_str(net->pass, sizeof(net->pass), value, report, true, key);
    }
}

static void apply_key(mosu_config_t *cfg, section_t section, const char *key,
                      const char *value, mosu_config_report_t *report)
{
    int ival = 0;
    long lval = 0;
    bool bval = false;
    uint32_t color = 0;

    switch (section) {
    case SEC_DEVICE:
        if (!strcasecmp(key, "name")) {
            copy_str(cfg->device.name, sizeof(cfg->device.name), value);
        } else if (!strcasecmp(key, "log_level")) {
            if (!strcasecmp(value, "none")) {
                cfg->device.log_level = 0;
            } else if (!strcasecmp(value, "error")) {
                cfg->device.log_level = 1;
            } else if (!strcasecmp(value, "warn")) {
                cfg->device.log_level = 2;
            } else if (!strcasecmp(value, "info")) {
                cfg->device.log_level = 3;
            } else if (!strcasecmp(value, "debug")) {
                cfg->device.log_level = 4;
            } else if (!strcasecmp(value, "verbose")) {
                cfg->device.log_level = 5;
            } else {
                report_warn(report, "unknown log level: %s", key);
                return;
            }
        } else {
            report_warn(report, "unknown key: %s", key);
            return;
        }
        break;

    case SEC_DISPLAY:
        if (!strcasecmp(key, "brightness")) {
            if (!set_int(value, 1, 100, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->display.brightness = (uint8_t)ival;
        } else if (!strcasecmp(key, "screen_on_ms")) {
            if (!parse_long(value, 1000, 300000, &lval)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->display.screen_on_ms = (uint32_t)lval;
        } else if (!strcasecmp(key, "wake_on_touch")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->display.wake_on_touch = bval;
        } else if (!strcasecmp(key, "wake_on_motion")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->display.wake_on_motion = bval;
        } else if (!strcasecmp(key, "motion_angle_deg")) {
            if (!set_int(value, 10, 80, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->display.motion_angle_deg = (uint16_t)ival;
        } else if (!strcasecmp(key, "motion_refractory_ms")) {
            if (!set_int(value, 300, 20000, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->display.motion_refractory_ms = (uint16_t)ival;
        } else if (!strcasecmp(key, "motion_poll_hz")) {
            if (!set_int(value, 1, 20, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->display.motion_poll_hz = (uint8_t)ival;
        } else if (!strcasecmp(key, "pixel_shift_sec")) {
            if (!set_int(value, 0, 3600, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->display.pixel_shift_sec = (uint16_t)ival;
        } else if (!strcasecmp(key, "tap_feedback_ms")) {
            if (!set_int(value, 0, 2000, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->display.tap_feedback_ms = (uint16_t)ival;
        } else {
            report_warn(report, "unknown key: %s", key);
            return;
        }
        break;

    case SEC_COLORS: {
        uint32_t *target = NULL;
        if (!strcasecmp(key, "background")) {
            target = &cfg->colors.background;
        } else if (!strcasecmp(key, "time_am")) {
            target = &cfg->colors.time_am;
        } else if (!strcasecmp(key, "time_pm")) {
            target = &cfg->colors.time_pm;
        } else if (!strcasecmp(key, "battery_green")) {
            target = &cfg->colors.battery_green;
        } else if (!strcasecmp(key, "battery_orange")) {
            target = &cfg->colors.battery_orange;
        } else if (!strcasecmp(key, "battery_red")) {
            target = &cfg->colors.battery_red;
        } else if (!strcasecmp(key, "wifi_idle")) {
            target = &cfg->colors.wifi_idle;
        } else if (!strcasecmp(key, "wifi_ok")) {
            target = &cfg->colors.wifi_ok;
        } else if (!strcasecmp(key, "wifi_fail")) {
            target = &cfg->colors.wifi_fail;
        } else if (!strcasecmp(key, "tap_border")) {
            target = &cfg->colors.tap_border;
        } else {
            report_warn(report, "unknown key: %s", key);
            return;
        }
        if (!parse_color(value, &color)) {
            report_error(report, "invalid colour for %s", key);
            return;
        }
        *target = color;
        break;
    }

    case SEC_STATUS:
        if (!strcasecmp(key, "battery_green_min")) {
            if (!set_int(value, 1, 100, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->status.battery_green_min = (uint8_t)ival;
        } else if (!strcasecmp(key, "battery_orange_min")) {
            if (!set_int(value, 1, 100, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->status.battery_orange_min = (uint8_t)ival;
        } else if (!strcasecmp(key, "show_battery")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->status.show_battery = bval;
        } else if (!strcasecmp(key, "show_wifi")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->status.show_wifi = bval;
        } else if (!strcasecmp(key, "wifi_fail_slash")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->status.wifi_fail_slash = bval;
        } else {
            report_warn(report, "unknown key: %s", key);
            return;
        }
        break;

    case SEC_CLOCK:
        if (!strcasecmp(key, "timezone")) {
            if (!mosu_config_timezone_ok(value)) {
                report_error(report, "invalid timezone for %s", key);
                return;
            }
            copy_str(cfg->clock.timezone, sizeof(cfg->clock.timezone), value);
        } else if (!strcasecmp(key, "auto_timezone")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->clock.auto_timezone = bval;
        } else if (!strcasecmp(key, "geo_url")) {
            copy_str(cfg->clock.geo_url, sizeof(cfg->clock.geo_url), value);
        } else if (!strcasecmp(key, "ntp1")) {
            copy_str(cfg->clock.ntp1, sizeof(cfg->clock.ntp1), value);
        } else if (!strcasecmp(key, "ntp2")) {
            copy_str(cfg->clock.ntp2, sizeof(cfg->clock.ntp2), value);
        } else if (!strcasecmp(key, "sync_hours")) {
            if (!set_int(value, 1, 168, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->clock.sync_hours = (uint16_t)ival;
        } else if (!strcasecmp(key, "wifi_always_on")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->clock.wifi_always_on = bval;
        } else if (!strcasecmp(key, "manual_time")) {
            copy_str(cfg->clock.manual_time, sizeof(cfg->clock.manual_time),
                     value);
        } else {
            report_warn(report, "unknown key: %s", key);
            return;
        }
        break;

    case SEC_WIFI:
        parse_wifi_key(cfg, key, value, report);
        break;

    case SEC_ANNOUNCE:
        if (!strcasecmp(key, "enabled")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->announce.enabled = bval;
        } else if (!strcasecmp(key, "double_tap_ms")) {
            if (!set_int(value, 200, 1500, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->announce.double_tap_ms = (uint16_t)ival;
        } else if (!strcasecmp(key, "max_hold_ms")) {
            if (!set_int(value, 80, 1500, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->announce.max_hold_ms = (uint16_t)ival;
        } else if (!strcasecmp(key, "max_move_px")) {
            if (!set_int(value, 4, 200, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->announce.max_move_px = (uint16_t)ival;
        } else if (!strcasecmp(key, "cooldown_ms")) {
            if (!set_int(value, 300, 10000, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->announce.cooldown_ms = (uint16_t)ival;
        } else if (!strcasecmp(key, "volume")) {
            if (!set_int(value, 0, 100, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->announce.volume = (uint8_t)ival;
        } else if (!strcasecmp(key, "announce_day")) {
            if (!parse_bool(value, &bval)) {
                report_error(report, "invalid boolean for %s", key);
                return;
            }
            cfg->announce.announce_day = bval;
        } else if (!strcasecmp(key, "voice_dir")) {
            copy_str(cfg->announce.voice_dir, sizeof(cfg->announce.voice_dir),
                     value);
        } else if (!strcasecmp(key, "clip_gap_ms")) {
            if (!set_int(value, 0, 1000, &ival)) {
                report_error(report, "invalid number for %s", key);
                return;
            }
            cfg->announce.clip_gap_ms = (uint16_t)ival;
        } else {
            report_warn(report, "unknown key: %s", key);
            return;
        }
        break;

    default:
        report_warn(report, "key outside of a section: %s", key);
        return;
    }

    if (report) {
        report->applied++;
    }
}

int mosu_config_parse(mosu_config_t *cfg, const char *text, size_t len,
                      mosu_config_report_t *report)
{
    if (!cfg || !text) {
        return -1;
    }
    if (report) {
        memset(report, 0, sizeof(*report));
    }

    section_t section = SEC_NONE;
    const char *cursor = text;
    const char *end = text + len;

    /* Editors on Windows like to prefix the file with a UTF-8 BOM.  It is not
     * part of the first key, so skip it once, at the document start only. */
    if ((size_t)(end - cursor) >= 3 && (unsigned char)cursor[0] == 0xEF &&
        (unsigned char)cursor[1] == 0xBB && (unsigned char)cursor[2] == 0xBF) {
        cursor += 3;
    }

    while (cursor < end) {
        const char *eol = memchr(cursor, '\n', (size_t)(end - cursor));
        size_t line_len = eol ? (size_t)(eol - cursor) : (size_t)(end - cursor);

        if (line_len >= LINE_MAX_LEN) {
            report_warn(report, "line too long near %s", "");
            line_len = LINE_MAX_LEN - 1;
        }

        char line[LINE_MAX_LEN];
        memcpy(line, cursor, line_len);
        line[line_len] = '\0';

        cursor = eol ? eol + 1 : end;

        if (report) {
            report->lines++;
        }

        char *work = trim(line);
        if (*work == '\0') {
            continue;
        }
        strip_comment(work);
        work = trim(work);
        if (*work == '\0') {
            continue;
        }

        if (*work == '[') {
            char *close = strchr(work + 1, ']');
            if (!close) {
                report_error(report, "unterminated section: %s", work);
                continue;
            }
            *close = '\0';
            section = section_from(trim(work + 1));
            if (section == SEC_NONE) {
                report_warn(report, "unknown section: %s", work + 1);
            }
            continue;
        }

        char *eq = strchr(work, '=');
        if (!eq) {
            report_warn(report, "line without '=': %s", work);
            continue;
        }

        *eq = '\0';
        char *key = trim(work);
        char *value = trim(eq + 1);
        unquote(value);

        if (*key == '\0') {
            report_warn(report, "empty key near %s", work);
            continue;
        }
        if (section == SEC_NONE) {
            report_warn(report, "key outside of a known section: %s", key);
            continue;
        }

        apply_key(cfg, section, key, value, report);
    }

    mosu_config_sanitize(cfg);
    return 0;
}

int mosu_config_load(mosu_config_t *cfg, const char *path,
                     mosu_config_report_t *report)
{
    if (!cfg || !path) {
        return -1;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return -1;
    }

    static char buffer[8192];
    size_t read = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[read] = '\0';

    return mosu_config_parse(cfg, buffer, read, report);
}

void mosu_config_sanitize(mosu_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    if (cfg->display.brightness < 1) {
        cfg->display.brightness = 1;
    }
    if (cfg->display.brightness > 100) {
        cfg->display.brightness = 100;
    }
    if (cfg->display.screen_on_ms < 1000) {
        cfg->display.screen_on_ms = 1000;
    }
    if (cfg->display.screen_on_ms > 300000) {
        cfg->display.screen_on_ms = 300000;
    }
    if (cfg->display.motion_angle_deg < 10) {
        cfg->display.motion_angle_deg = 10;
    }
    if (cfg->display.motion_angle_deg > 80) {
        cfg->display.motion_angle_deg = 80;
    }
    if (cfg->display.motion_poll_hz < 1) {
        cfg->display.motion_poll_hz = 1;
    }
    if (cfg->display.motion_poll_hz > 20) {
        cfg->display.motion_poll_hz = 20;
    }
    if (cfg->display.motion_refractory_ms < 300) {
        cfg->display.motion_refractory_ms = 300;
    }

    if (cfg->status.battery_green_min > 100) {
        cfg->status.battery_green_min = 100;
    }
    if (cfg->status.battery_orange_min > cfg->status.battery_green_min) {
        cfg->status.battery_orange_min = cfg->status.battery_green_min;
    }

    if (cfg->clock.sync_hours < 1) {
        cfg->clock.sync_hours = 1;
    }
    if (cfg->clock.sync_hours > 168) {
        cfg->clock.sync_hours = 168;
    }
    if (!mosu_config_timezone_ok(cfg->clock.timezone)) {
        copy_str(cfg->clock.timezone, sizeof(cfg->clock.timezone),
                 "EET-2EEST,M3.5.0/3,M10.5.0/4");
    }

    if (cfg->announce.volume > 100) {
        cfg->announce.volume = 100;
    }
    if (cfg->announce.double_tap_ms < 200) {
        cfg->announce.double_tap_ms = 200;
    }
    if (cfg->announce.cooldown_ms < 300) {
        cfg->announce.cooldown_ms = 300;
    }

    /* Compact the network list so network2 works without network1. */
    mosu_wifi_cfg_t compact[MOSU_CFG_WIFI_MAX];
    uint8_t count = 0;
    memset(compact, 0, sizeof(compact));
    for (uint8_t i = 0; i < MOSU_CFG_WIFI_MAX; i++) {
        if (cfg->wifi[i].ssid[0] != '\0') {
            compact[count++] = cfg->wifi[i];
        }
    }
    memcpy(cfg->wifi, compact, sizeof(compact));
    cfg->wifi_count = count;
}

bool mosu_config_timezone_ok(const char *tz)
{
    if (!tz) {
        return false;
    }
    size_t len = strlen(tz);
    if (len == 0 || len >= MOSU_CFG_TZ_MAX) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = tz[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '+' || c == '-' ||
              c == '/' || c == ':' || c == ',' || c == '.')) {
            return false;
        }
    }

    /* Reject strings that look like an option or are only punctuation. */
    if (tz[0] == '-' || tz[0] == ':' || tz[0] == ',' || tz[0] == '/') {
        return false;
    }
    return true;
}
