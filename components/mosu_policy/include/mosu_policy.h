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
 * normal sync_hours interval takes over again.  sync_hours is clamped to at
 * least one hour so a bad config can never busy-loop the radio.
 */
uint32_t mosu_policy_next_sync_s(uint8_t fail_count, uint16_t sync_hours);

#ifdef __cplusplus
}
#endif
