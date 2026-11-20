/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Pure C policy helpers.  No ESP-IDF headers: the host tests exercise exactly
 * this file.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "mosu_policy.h"

#define OFFSET_MIN_S (-12 * 3600)
#define OFFSET_MAX_S (14 * 3600)

/* Values that are small enough to be hours, and large enough to be seconds. */
#define HOURS_MAX_ABS 18
#define SECONDS_MIN_ABS 60

/* Anything bigger is nonsense and is rejected before it can overflow. */
#define MAX_WHOLE 1000000LL

static const uint32_t s_backoff_s[MOSU_POLICY_SYNC_RETRIES] = {
    5 * 60, 15 * 60, 30 * 60,
};

bool mosu_policy_is_offset(const char *text)
{
    if (!text || *text == '\0') {
        return false;
    }

    const char *p = text;
    if (*p == '+' || *p == '-') {
        p++;
    }

    bool have_digit = false;
    bool have_dot = false;
    bool have_fraction = false;

    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            have_digit = true;
            if (have_dot) {
                have_fraction = true;
            }
        } else if (*p == '.' && !have_dot) {
            have_dot = true;
        } else {
            return false;
        }
    }

    return have_digit && (!have_dot || have_fraction);
}

static bool parse_scaled(const char *text, long long *out, bool *has_dot)
{
    const char *p = text;
    int sign = 1;

    if (*p == '+') {
        p++;
    } else if (*p == '-') {
        sign = -1;
        p++;
    }

    long long whole = 0;
    bool too_big = false;
    while (*p >= '0' && *p <= '9') {
        if (!too_big) {
            whole = whole * 10 + (*p - '0');
            too_big = whole > MAX_WHOLE;
        }
        p++;
    }
    if (too_big) {
        return false;
    }

    int frac = 0;
    int digits = 0;
    *has_dot = false;
    if (*p == '.') {
        *has_dot = true;
        p++;
        while (*p >= '0' && *p <= '9') {
            if (digits < 3) {
                frac = frac * 10 + (*p - '0');
                digits++;
            }
            p++;
        }
    }
    while (digits < 3) {
        frac *= 10;
        digits++;
    }

    *out = sign * (whole * 1000 + frac);
    return true;
}

static bool write_posix(long long seconds, char *out, size_t out_len)
{
    if (seconds == 0) {
        int n = snprintf(out, out_len, "UTC0");
        return n > 0 && (size_t)n < out_len;
    }

    /* POSIX inverts the sign: east of Greenwich is "UTC-2". */
    char sign = seconds > 0 ? '-' : '+';
    unsigned total = (unsigned)(seconds > 0 ? seconds : -seconds);
    unsigned hours = total / 3600u;
    unsigned minutes = (total % 3600u) / 60u;
    unsigned rest = total % 60u;

    int n;
    if (rest != 0) {
        n = snprintf(out, out_len, "UTC%c%u:%02u:%02u", sign, hours, minutes,
                     rest);
    } else if (minutes != 0) {
        n = snprintf(out, out_len, "UTC%c%u:%02u", sign, hours, minutes);
    } else {
        n = snprintf(out, out_len, "UTC%c%u", sign, hours);
    }
    return n > 0 && (size_t)n < out_len;
}

bool mosu_policy_offset_to_posix(const char *text, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    if (!mosu_policy_is_offset(text)) {
        return false;
    }

    long long scaled = 0;
    bool has_dot = false;
    if (!parse_scaled(text, &scaled, &has_dot)) {
        return false;
    }

    long long magnitude = scaled < 0 ? -scaled : scaled;
    bool in_hours;
    if (has_dot || magnitude <= HOURS_MAX_ABS * 1000LL) {
        in_hours = true;
    } else if (magnitude >= SECONDS_MIN_ABS * 1000LL) {
        in_hours = false;
    } else {
        /* 19..59 without a decimal point could be either unit. */
        return false;
    }

    long long seconds = in_hours ? (scaled * 3600) / 1000 : scaled / 1000;
    if (seconds < OFFSET_MIN_S || seconds > OFFSET_MAX_S) {
        return false;
    }

    return write_posix(seconds, out, out_len);
}

bool mosu_policy_zone_plausible(const char *zone)
{
    if (!zone || *zone == '\0') {
        return false;
    }

    size_t len = strlen(zone);
    if (len >= MOSU_POLICY_TZ_MAX) {
        return false;
    }

    bool have_alpha = false;
    bool have_digit = false;
    for (size_t i = 0; i < len; i++) {
        char c = zone[i];
        if (isalpha((unsigned char)c)) {
            have_alpha = true;
        } else if (isdigit((unsigned char)c)) {
            have_digit = true;
        } else if (c != '_' && c != '+' && c != '-' && c != '/' && c != ':' &&
                   c != ',' && c != '.') {
            return false;
        }
    }

    if (!have_alpha) {
        return false;
    }
    if (zone[0] == '-' || zone[0] == ':' || zone[0] == ',' || zone[0] == '/') {
        return false;
    }

    if (strchr(zone, '/')) {
        /* IANA name: no empty path component. */
        return zone[len - 1] != '/' && strstr(zone, "//") == NULL;
    }

    /* A POSIX TZ string always carries an offset digit. */
    if (have_digit) {
        return true;
    }

    static const char *const abbreviations[] = {
        "UTC", "GMT", "CET", "EET", "WET", "MET", "EST", "CST", "MST", "PST",
    };
    for (size_t i = 0; i < sizeof(abbreviations) / sizeof(abbreviations[0]);
         i++) {
        if (!strcasecmp(zone, abbreviations[i])) {
            return true;
        }
    }
    return false;
}

static void copy_span(const char *begin, const char *end, char *dst,
                      size_t dst_len)
{
    while (begin < end && isspace((unsigned char)*begin)) {
        begin++;
    }
    while (end > begin && isspace((unsigned char)end[-1])) {
        end--;
    }

    size_t len = (size_t)(end - begin);
    dst[0] = '\0';
    if (len == 0 || len >= dst_len) {
        return;
    }
    memcpy(dst, begin, len);
    dst[len] = '\0';
}

static void parse_json_zone(const char *body, mosu_policy_geo_t *out)
{
    const char *key = strstr(body, "\"timezone\"");
    if (!key) {
        return;
    }

    const char *colon = strchr(key, ':');
    if (!colon) {
        return;
    }

    const char *p = colon + 1;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '"') {
        return;
    }

    const char *start = p + 1;
    const char *end = strchr(start, '"');
    if (!end) {
        return;
    }

    char zone[MOSU_POLICY_TZ_MAX];
    copy_span(start, end, zone, sizeof(zone));
    if (mosu_policy_zone_plausible(zone)) {
        memcpy(out->zone, zone, sizeof(out->zone));
    }
}

static void parse_json_offset(const char *body, mosu_policy_geo_t *out)
{
    const char *key = strstr(body, "\"offset\"");
    if (!key) {
        return;
    }

    const char *colon = strchr(key, ':');
    if (!colon) {
        return;
    }

    const char *p = colon + 1;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '"') {
        p++;
    }

    const char *start = p;
    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '}' &&
           *p != '"') {
        p++;
    }

    char token[24];
    copy_span(start, p, token, sizeof(token));
    if (token[0]) {
        mosu_policy_offset_to_posix(token, out->posix, sizeof(out->posix));
    }
}

static void parse_lines(const char *body, mosu_policy_geo_t *out)
{
    const char *line = body;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) {
            end = line + strlen(line);
        }

        char token[MOSU_POLICY_TZ_MAX];
        copy_span(line, end, token, sizeof(token));
        if (token[0]) {
            if (!out->posix[0] &&
                mosu_policy_offset_to_posix(token, out->posix,
                                            sizeof(out->posix))) {
                /* Numeric answer. */
            } else if (!out->zone[0] && mosu_policy_zone_plausible(token)) {
                memcpy(out->zone, token, sizeof(out->zone));
            }
        }

        if (*end == '\0') {
            break;
        }
        line = end + 1;
    }
}

void mosu_policy_parse_geo(const char *body, mosu_policy_geo_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!body) {
        return;
    }

    parse_json_zone(body, out);
    parse_json_offset(body, out);
    parse_lines(body, out);
}

uint32_t mosu_policy_next_sync_s(uint8_t fail_count, uint32_t sync_interval_s)
{
    if (fail_count > 0 && fail_count <= MOSU_POLICY_SYNC_RETRIES) {
        return s_backoff_s[fail_count - 1];
    }

    return sync_interval_s < 3600u ? 3600u : sync_interval_s;
}

/* --------------------------------------------------------- learned policy */

void mosu_wifi_learn_init(mosu_wifi_learn_t *learn)
{
    if (!learn) {
        return;
    }
    for (int i = 0; i < MOSU_WIFI_HOURS; i++) {
        learn->score[i] = 128;
    }
}

void mosu_wifi_learn_note(mosu_wifi_learn_t *learn, uint8_t hour, bool ok)
{
    if (!learn || hour >= MOSU_WIFI_HOURS) {
        return;
    }
    uint8_t score = learn->score[hour];
    if (ok) {
        score = (uint8_t)(score + (255 - score) / 4 + 1);
    } else {
        score = (uint8_t)(score - score / 8 - 1);
    }
    learn->score[hour] = score;
}

uint8_t mosu_wifi_learn_score(const mosu_wifi_learn_t *learn, uint8_t hour)
{
    if (!learn || hour >= MOSU_WIFI_HOURS) {
        return 128;
    }
    return learn->score[hour];
}

uint32_t mosu_wifi_learn_scan_s(const mosu_wifi_learn_t *learn, uint8_t hour)
{
    uint8_t score = mosu_wifi_learn_score(learn, hour);
    if (score >= 160) {
        return 15u * 60u;
    }
    if (score >= 96) {
        return 30u * 60u;
    }
    if (score >= 48) {
        return 60u * 60u;
    }
    if (score >= 24) {
        return 120u * 60u;
    }
    return 240u * 60u;
}

void mosu_power_learn_init(mosu_power_learn_t *learn)
{
    if (!learn) {
        return;
    }
    memset(learn, 0, sizeof(*learn));
}

void mosu_power_learn_note(mosu_power_learn_t *learn, uint32_t dt_ms,
                           bool screen_on)
{
    if (!learn) {
        return;
    }
    learn->window_ms += dt_ms;
    if (screen_on) {
        learn->screen_ms += dt_ms;
        if (!learn->was_on) {
            learn->wakes++;
        }
    }
    learn->was_on = screen_on;
}

bool mosu_power_learn_tick(mosu_power_learn_t *learn, uint32_t window_ms)
{
    if (!learn || learn->window_ms == 0) {
        return false;
    }

    const uint32_t elapsed = learn->window_ms;
    const uint32_t duty_per_mille = (uint32_t)((uint64_t)learn->screen_ms *
                                               1000u / elapsed);
    bool changed = false;

    learn->since_change_ms += elapsed;

    if ((duty_per_mille >= 120u || learn->wakes >= 10u) &&
        learn->since_change_ms >= 3u * 3600u * 1000u) {
        uint8_t bonus = (uint8_t)(learn->angle_bonus + 2u);
        if (bonus > MOSU_POWER_ANGLE_MAX) {
            bonus = MOSU_POWER_ANGLE_MAX;
        }
        uint16_t cut = (uint16_t)(learn->screen_cut_ms + 1000u);
        if (cut > MOSU_POWER_CUT_MAX_MS) {
            cut = MOSU_POWER_CUT_MAX_MS;
        }
        changed = bonus != learn->angle_bonus || cut != learn->screen_cut_ms;
        learn->angle_bonus = bonus;
        learn->screen_cut_ms = cut;
        learn->calm_hours = 0;
        learn->since_change_ms = 0;
    } else if (duty_per_mille <= 30u && learn->wakes <= 3u) {
        if (learn->calm_hours < UINT8_MAX) {
            learn->calm_hours++;
        }
        if (learn->calm_hours >= 3u &&
            learn->since_change_ms >= 6u * 3600u * 1000u) {
            if (learn->angle_bonus > 0) {
                learn->angle_bonus--;
            }
            if (learn->screen_cut_ms >= 500u) {
                learn->screen_cut_ms = (uint16_t)(learn->screen_cut_ms - 500u);
            } else {
                learn->screen_cut_ms = 0;
            }
            learn->calm_hours = 0;
            learn->since_change_ms = 0;
            changed = true;
        }
    } else {
        learn->calm_hours = 0;
    }

    learn->window_ms = 0;
    learn->screen_ms = 0;
    learn->wakes = 0;
    (void)window_ms;
    return changed;
}

uint16_t mosu_power_learn_angle(const mosu_power_learn_t *learn,
                                uint16_t base_deg)
{
    uint32_t angle = base_deg;
    if (learn) {
        angle += learn->angle_bonus;
    }
    return angle > 80u ? 80u : (uint16_t)angle;
}

uint32_t mosu_power_learn_screen_ms(const mosu_power_learn_t *learn,
                                    uint32_t base_ms)
{
    if (!learn) {
        return base_ms;
    }
    if (base_ms <= learn->screen_cut_ms) {
        return MOSU_POWER_SCREEN_MIN_MS;
    }
    uint32_t result = base_ms - learn->screen_cut_ms;
    return result < MOSU_POWER_SCREEN_MIN_MS ? MOSU_POWER_SCREEN_MIN_MS
                                             : result;
}
