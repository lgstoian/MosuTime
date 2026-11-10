/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "mosu_core/mosu_core.h"

void mosu_time_split12(uint8_t hour24, uint8_t *hour12, bool *pm)
{
    uint8_t h = hour24 % 24;
    bool is_pm = h >= 12;
    uint8_t h12 = h % 12;

    if (h12 == 0) {
        h12 = 12;
    }
    if (hour12) {
        *hour12 = h12;
    }
    if (pm) {
        *pm = is_pm;
    }
}

void mosu_format_clock(uint8_t hour24, uint8_t minute, bool valid,
                       char hours[3], char minutes[3])
{
    if (!valid) {
        hours[0] = '-';
        hours[1] = '-';
        hours[2] = '\0';
        minutes[0] = '-';
        minutes[1] = '-';
        minutes[2] = '\0';
        return;
    }

    uint8_t h12;
    mosu_time_split12(hour24, &h12, NULL);
    if (h12 >= 10) {
        hours[0] = (char)('0' + h12 / 10);
        hours[1] = (char)('0' + h12 % 10);
        hours[2] = '\0';
    } else {
        hours[0] = (char)('0' + h12);
        hours[1] = '\0';
        hours[2] = '\0';
    }

    uint8_t m = (uint8_t)(minute % 60);
    minutes[0] = (char)('0' + m / 10);
    minutes[1] = (char)('0' + m % 10);
    minutes[2] = '\0';
}
