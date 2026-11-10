/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Wrist raise detection from the accelerometer alone.
 *
 * A raise is a large, *stable* change of the gravity direction.  Requiring the
 * new orientation to persist for a moment removes almost all false positives
 * from ordinary hand movements and from setting the arm down.
 */
#include <math.h>
#include <string.h>

#include "mosu_core/mosu_core.h"

#define MOSU_WRIST_HOLD_MS 150
#define MOSU_WRIST_HOLD_DEG 25.0f
#define MOSU_WRIST_STILL_DEG 12.0f
#define MOSU_WRIST_BASELINE_ALPHA 0.05f
#define MOSU_RAD_TO_DEG 57.2957795f

void mosu_wrist_init(mosu_wrist_t *wrist)
{
    if (!wrist) {
        return;
    }
    memset(wrist, 0, sizeof(*wrist));
}

void mosu_wrist_reset(mosu_wrist_t *wrist)
{
    if (!wrist) {
        return;
    }
    wrist->have_baseline = false;
    wrist->have_candidate = false;
    wrist->candidate_ms = 0;
}

static float angle_between(float ax, float ay, float az, float bx, float by,
                           float bz)
{
    float dot = ax * bx + ay * by + az * bz;
    if (dot > 1.0f) {
        dot = 1.0f;
    } else if (dot < -1.0f) {
        dot = -1.0f;
    }
    return acosf(dot) * MOSU_RAD_TO_DEG;
}

bool mosu_wrist_update(mosu_wrist_t *wrist, const mosu_accel_t *accel,
                       const mosu_wrist_cfg_t *cfg, uint32_t now_ms)
{
    if (!wrist || !accel || !cfg) {
        return false;
    }

    float magnitude = sqrtf(accel->x * accel->x + accel->y * accel->y +
                            accel->z * accel->z);
    if (magnitude < 0.2f) {
        return false;
    }

    float nx = accel->x / magnitude;
    float ny = accel->y / magnitude;
    float nz = accel->z / magnitude;

    if (!wrist->have_baseline) {
        wrist->bx = nx;
        wrist->by = ny;
        wrist->bz = nz;
        wrist->have_baseline = true;
        return false;
    }

    if ((int32_t)(now_ms - wrist->last_event_ms) <
        -(int32_t)cfg->refractory_ms) {
        wrist->last_event_ms = now_ms;
    }

    bool refractory =
        wrist->have_event &&
        (uint32_t)(now_ms - wrist->last_event_ms) < cfg->refractory_ms;

    float threshold = (float)cfg->angle_mdeg / 1000.0f;
    float from_baseline =
        angle_between(nx, ny, nz, wrist->bx, wrist->by, wrist->bz);

    if (!wrist->have_candidate) {
        if (!refractory && from_baseline >= threshold) {
            wrist->have_candidate = true;
            wrist->cx = nx;
            wrist->cy = ny;
            wrist->cz = nz;
            wrist->candidate_ms = now_ms;
        } else if (from_baseline < MOSU_WRIST_STILL_DEG) {
            /* Slow baseline tracking while the arm is basically still. */
            wrist->bx += (nx - wrist->bx) * MOSU_WRIST_BASELINE_ALPHA;
            wrist->by += (ny - wrist->by) * MOSU_WRIST_BASELINE_ALPHA;
            wrist->bz += (nz - wrist->bz) * MOSU_WRIST_BASELINE_ALPHA;
            float len = sqrtf(wrist->bx * wrist->bx + wrist->by * wrist->by +
                              wrist->bz * wrist->bz);
            if (len > 0.001f) {
                wrist->bx /= len;
                wrist->by /= len;
                wrist->bz /= len;
            }
        }
        return false;
    }

    float from_candidate =
        angle_between(nx, ny, nz, wrist->cx, wrist->cy, wrist->cz);

    if (from_candidate <= MOSU_WRIST_HOLD_DEG) {
        if ((uint32_t)(now_ms - wrist->candidate_ms) >= MOSU_WRIST_HOLD_MS &&
            !refractory) {
            /* The new orientation held: this is a raise. */
            wrist->bx = wrist->cx;
            wrist->by = wrist->cy;
            wrist->bz = wrist->cz;
            wrist->have_candidate = false;
            wrist->last_event_ms = now_ms;
            wrist->have_event = true;
            return true;
        }
        return false;
    }

    /* The arm kept moving: restart the candidate at the current orientation. */
    wrist->cx = nx;
    wrist->cy = ny;
    wrist->cz = nz;
    wrist->candidate_ms = now_ms;
    return false;
}
