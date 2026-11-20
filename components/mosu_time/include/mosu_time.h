/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"
#include "mosu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mosu_time_init(const mosu_config_t *cfg);

/* Local time, already adjusted for the active timezone and DST. */
void mosu_time_now(struct tm *out);

/* False until the clock has a believable date (RTC, manual or SNTP). */
bool mosu_time_valid(void);
/* Apply an SNTP result.  Also writes the RTC, in UTC. */
esp_err_t mosu_time_set_epoch(time_t utc);

/*
 * Set the clock without claiming that the network supplied it: used by the
 * console's `settime`.  The Wi-Fi icon keeps showing the last real sync.
 */
esp_err_t mosu_time_set_epoch_manual(time_t utc);

/*
 * Apply an IANA name ("Europe/Bucharest"), a POSIX TZ string or a numeric UTC
 * offset ("7200", "+2", "-5.5").  IANA names are mapped to the POSIX form
 * newlib understands; offsets are spelled as POSIX before they are stored.
 * The chosen zone is remembered in NVS.
 */
bool mosu_time_apply_timezone(const char *tz);

const char *mosu_time_timezone(void);

/* Seconds since the last successful SNTP sync, UINT32_MAX when never. */
uint32_t mosu_time_sync_age_s(void);

esp_err_t mosu_time_persist_rtc(void);

#ifdef __cplusplus
}
#endif
