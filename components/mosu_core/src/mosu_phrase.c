/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "mosu_core/mosu_core.h"

void mosu_phrase_build(uint8_t hour24, uint8_t minute, uint8_t weekday,
                       bool announce_day, mosu_phrase_t *out)
{
    if (!out) {
        return;
    }

    out->count = 0;
    out->valid = false;

    uint8_t h12 = 12;
    bool pm = false;
    mosu_time_split12(hour24, &h12, &pm);

    out->hour12 = h12;
    out->pm = pm;
    out->valid = true;

    out->clips[out->count].kind = MOSU_CLIP_HOUR;
    out->clips[out->count].index = h12;
    out->count++;

    if ((minute % 60) != 0) {
        out->clips[out->count].kind = MOSU_CLIP_MINUTE;
        out->clips[out->count].index = (uint8_t)(minute % 60);
        out->count++;
    }

    if (announce_day) {
        out->clips[out->count].kind = MOSU_CLIP_DAY;
        out->clips[out->count].index = (uint8_t)(weekday % 7);
        out->count++;
    }
}

uint8_t mosu_weekday_from_tm(int tm_wday)
{
    /* struct tm: 0 = Sunday.  Clips: 0 = Monday. */
    int day = (tm_wday + 6) % 7;
    if (day < 0) {
        day += 7;
    }
    return (uint8_t)day;
}

bool mosu_clip_filename(mosu_clip_kind_t kind, uint8_t index, char *buf,
                        size_t len)
{
    const char *prefix;
    unsigned max_index;

    switch (kind) {
    case MOSU_CLIP_HOUR:
        prefix = "h";
        max_index = 12;
        break;
    case MOSU_CLIP_MINUTE:
        prefix = "m";
        max_index = 59;
        break;
    case MOSU_CLIP_DAY:
        prefix = "d";
        max_index = 6;
        break;
    default:
        return false;
    }

    if (!buf || len == 0) {
        return false;
    }
    if (kind == MOSU_CLIP_DAY) {
        if (index > 6) {
            return false;
        }
    } else if (index == 0 || index > max_index) {
        return false;
    }

    int written = snprintf(buf, len, "%s_%u.wav", prefix, (unsigned)index);
    return written > 0 && (size_t)written < len;
}
