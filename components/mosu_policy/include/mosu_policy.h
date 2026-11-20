/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Pure C rules for the network side of the clock: turning a numeric UTC
 * offset into a POSIX TZ string, reading the geolocation answer and choosing
 * how long to wait after a failed sync.  Nothing here includes ESP-IDF, so the
 * host tests compile the same code the watch runs.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOSU_POLICY_TZ_MAX 64
#define MOSU_POLICY_SYNC_RETRIES 3
#define MOSU_WIFI_HOURS 24
#define MOSU_POWER_ANGLE_MAX 14
#define MOSU_POWER_CUT_MAX_MS 7000
#define MOSU_POWER_SCREEN_MIN_MS 8000

/*
 * True when the text is a bare numeric UTC offset: an optional sign followed
 * by digits, optionally with a decimal fraction.  Examples: "7200", "+7200",
 * "-18000", "2", "+2", "-5.5".
 */
bool mosu_policy_is_offset(const char *text);

/*
 * Spell a numeric UTC offset as a POSIX TZ string.
 *
 * The value is read as seconds when its magnitude is at least 60, and as
 * hours when it is at most 18 (a decimal point always means hours).  Values
 * between those ranges are ambiguous and rejected.  The result uses the POSIX
 * sign convention, which is inverted compared with the usual UTC offset:
 *
 *   "7200"   -> "UTC-2"
 *   "-18000" -> "UTC+5"
 *   "-5.5"   -> "UTC+5:30"
 *   "19800"  -> "UTC-5:30"
 *
 * Returns false for anything that is not a plausible offset, for values
 * outside -12:00..+14:00, and when the output buffer is too small.
 */
bool mosu_policy_offset_to_posix(const char *text, char *out, size_t out_len);

/*
 * True when the string is safe to hand to setenv("TZ", ...) and actually looks
 * like a zone: either an IANA-style name with a slash, a POSIX form with a
 * numeric offset, or a short well-known abbreviation.  Error words such as
 * "fail" and HTML fragments are rejected.
 */
bool mosu_policy_zone_plausible(const char *zone);

typedef struct {
    char zone[MOSU_POLICY_TZ_MAX];  /* plausible name, or "" */
    char posix[MOSU_POLICY_TZ_MAX]; /* numeric offset as POSIX TZ, or "" */
} mosu_policy_geo_t;

/*
 * Parse a geolocation response.  Three shapes are understood:
 *
 *   "Europe/Bucharest\n7200\n"                     name + offset, two lines
 *   {"timezone":"Europe/Bucharest","offset":7200}  ip-api JSON
 *   "Europe/Bucharest\n"                           plain zone name
 *
 * Either output may stay empty.  Callers prefer the name when they can map
 * it, and fall back to the POSIX offset otherwise.
 */
void mosu_policy_parse_geo(const char *body, mosu_policy_geo_t *out);

/*
 * Seconds until the next sync attempt.
 *
 * fail_count is the number of consecutive failures so far, 0 after a success.
 * The first three failures retry after 5, 15 and 30 minutes; after that the
 * normal interval takes over again.  sync_interval_s is clamped to at least
 * one hour so a bad config can never busy-loop the radio.
 */
uint32_t mosu_policy_next_sync_s(uint8_t fail_count,
                                 uint32_t sync_interval_s);

/*
 * Learned Wi-Fi schedule.
 *
 * The radio is off between syncs, but a failed sync starts presence scans.
 * Scanning every 15 minutes all day is wasteful when the watch is away from
 * home: the score per local hour learns which hours actually have the access
 * point in range, and the scan interval follows it.  Scores are neutral (128)
 * at first, rise quickly on success and fall slowly on failure.
 */
typedef struct {
    uint8_t score[MOSU_WIFI_HOURS];
} mosu_wifi_learn_t;

void mosu_wifi_learn_init(mosu_wifi_learn_t *learn);
void mosu_wifi_learn_note(mosu_wifi_learn_t *learn, uint8_t hour, bool ok);
uint8_t mosu_wifi_learn_score(const mosu_wifi_learn_t *learn, uint8_t hour);
/* Seconds between presence scans for this hour: 15 min when the hour is
 * usually online, 1-6 h when it is rarely or never online. */
uint32_t mosu_wifi_learn_scan_s(const mosu_wifi_learn_t *learn, uint8_t hour);

/*
 * Learned screen policy.
 *
 * The battery cost is dominated by screen-on seconds, so the watch measures
 * its own duty cycle per hour.  A busy hour (>=12% lit or >=10 wakes) raises
 * the wrist threshold by a step and shortens the lit window; three calm hours
 * in a row give a step back.  The offsets are bounded: at most +14 degrees
 * and -7 s, never below an 8 s window.  The configured values stay the
 * baseline, so a reboot returns to them and the config file remains the
 * source of truth.
 */
typedef struct {
    uint8_t angle_bonus;    /* degrees above the configured angle, 0..14 */
    uint16_t screen_cut_ms; /* milliseconds below screen_on_ms, 0..7000 */
    uint32_t window_ms;     /* time accumulated in this window */
    uint32_t screen_ms;     /* lit time in this window */
    uint32_t wakes;         /* screen turns in this window */
    uint8_t calm_hours;
    uint32_t since_change_ms;
    bool was_on;
} mosu_power_learn_t;

void mosu_power_learn_init(mosu_power_learn_t *learn);
/* dt_ms since the previous call; screen_on is the current panel state. */
void mosu_power_learn_note(mosu_power_learn_t *learn, uint32_t dt_ms,
                           bool screen_on);
/* Close a window (normally one hour); true when the offsets changed. */
bool mosu_power_learn_tick(mosu_power_learn_t *learn, uint32_t window_ms);
uint16_t mosu_power_learn_angle(const mosu_power_learn_t *learn,
                                uint16_t base_deg);
uint32_t mosu_power_learn_screen_ms(const mosu_power_learn_t *learn,
                                    uint32_t base_ms);

#ifdef __cplusplus
}
#endif
