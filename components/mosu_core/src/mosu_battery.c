/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * A single cell LiPo is not linear.  The AXP2101 fuel gauge is famously
 * optimistic, so the watch derives its percentage from the measured cell
 * voltage and this curve, then smooths it in the application layer.
 */
#include "mosu_core/mosu_core.h"

typedef struct {
    uint16_t mv;
    uint8_t percent;
} curve_point_t;

/* Descending voltage, descending percentage.  The AXP2101 terminates at
 * 25 mA, so a full cell rests around 4.15-4.18 V, not 4.20 V.  Mapping 100%
 * to 4.20 V made the watch report 98% forever even when the charger had
 * finished; 4.16 V is the real full point for this profile. */
static const curve_point_t s_curve[] = {
    {4160, 100}, {4120, 95}, {4050, 88}, {3980, 80}, {3910, 70},
    {3850, 60},  {3790, 48}, {3730, 36}, {3680, 26}, {3620, 16},
    {3550, 9},   {3450, 4},  {3300, 0},
};

uint8_t mosu_battery_percent(uint16_t millivolts)
{
    const size_t n = MOSU_ARRAY_LEN(s_curve);

    if (millivolts >= s_curve[0].mv) {
        return 100;
    }
    if (millivolts <= s_curve[n - 1].mv) {
        return 0;
    }

    for (size_t i = 1; i < n; i++) {
        if (millivolts >= s_curve[i].mv) {
            uint16_t hi_mv = s_curve[i - 1].mv;
            uint16_t lo_mv = s_curve[i].mv;
            int hi_pc = s_curve[i - 1].percent;
            int lo_pc = s_curve[i].percent;
            int span_mv = (int)hi_mv - (int)lo_mv;
            int span_pc = hi_pc - lo_pc;
            int offset = (int)millivolts - (int)lo_mv;
            return (uint8_t)(lo_pc + (offset * span_pc) / span_mv);
        }
    }

    return 0;
}

mosu_batt_bucket_t mosu_battery_bucket(uint8_t percent, uint8_t green_min,
                                       uint8_t orange_min)
{
    if (percent >= green_min) {
        return MOSU_BATT_GREEN;
    }
    if (percent >= orange_min) {
        return MOSU_BATT_ORANGE;
    }
    return MOSU_BATT_RED;
}
