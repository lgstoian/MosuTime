/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Wrist raise detection from the accelerometer alone.
 *
 * The watch shows the time when the wearer turns the glass towards his face.
 * That is a large change of the gravity direction which then *holds* for a
 * moment; the direction is all an accelerometer can see, so the shape of the
 * movement is the signal:
 *
 *   1. the orientation departs from the resting baseline by a real angle,
 *   2. the new orientation stops changing (the arm has reached the look),
 *   3. it stays there for `hold_ms`,
 *   4. the screen normal is not vertical (the glass faces up, not the wall),
 *   5. the refractory window has passed.
 *
 * Stillness before the movement is still measured, but it is *not* a gate.
 * A continuous raise crosses the angle threshold while the arm is moving, and
 * a gate evaluated at that instant can never be satisfied; that bug is what
 * made the watch respond only to a hard shake.
 */
#include <math.h>
#include <string.h>

#include "mosu_core/mosu_core.h"

#define MOSU_WRIST_HOLD_MS 250
#define MOSU_WRIST_HOLD_DEG 18.0f
#define MOSU_WRIST_FACE_UP_MIN 0.20f
/* Sample-to-sample change that means "the arm is moving".  At 20 Hz a natural
 * raise moves about 8 degrees per sample, ordinary hand tremor about 1. */
#define MOSU_WRIST_MOVE_DEG 6.0f
/* Linear acceleration that means the arm is moving even when the direction
 * happens to be unchanged. */
#define MOSU_WRIST_MOVE_G 0.12f
/* Only follow the resting orientation once the arm has really settled, so a
 * slow raise is not absorbed into the baseline before it is recognised.  The
 * tracking also stops once the sample is this far from the baseline: at a high
 * sample rate a slow raise moves less than MOVE_DEG per sample and would
 * otherwise be tracked away. */
#define MOSU_WRIST_SETTLE_MS 500
#define MOSU_WRIST_TRACK_DEG 10.0f
/* A new orientation that is not a raise (glass vertical, or still inside the
 * refractory window) becomes the resting baseline after this long, otherwise
 * the detector would keep measuring against an old arm position forever. */
#define MOSU_WRIST_ADOPT_MS 1500
#define MOSU_WRIST_BASELINE_ALPHA 0.10f
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
    wrist->have_prev = false;
    wrist->have_candidate = false;
    wrist->moving = false;
    wrist->candidate_ms = 0;
    wrist->last_event_ms = 0;
    wrist->still_ms = 0;
    wrist->last_ms = 0;
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
        wrist->px = nx;
        wrist->py = ny;
        wrist->pz = nz;
        wrist->have_baseline = true;
        wrist->have_prev = true;
        wrist->last_ms = now_ms;
        return false;
    }

    uint32_t dt = wrist->last_ms ? (uint32_t)(now_ms - wrist->last_ms) : 0;
    wrist->last_ms = now_ms;
    if (dt > 1000u) {
        dt = 1000u;
    }

    /* Motion is a change from the previous sample, not a distance from the
     * baseline: an arm that swings while walking is moving even though it
     * keeps returning to roughly the same orientation. */
    float step = 0.0f;
    if (wrist->have_prev) {
        step = angle_between(nx, ny, nz, wrist->px, wrist->py, wrist->pz);
    }
    const bool motion =
        step >= MOSU_WRIST_MOVE_DEG || fabsf(magnitude - 1.0f) >= MOSU_WRIST_MOVE_G;

    if (motion) {
        wrist->moving = true;
        wrist->still_ms = 0;
    } else {
        wrist->moving = false;
        wrist->still_ms = wrist->still_ms + dt > 60000u ? 60000u
                                                        : wrist->still_ms + dt;
    }
    wrist->px = nx;
    wrist->py = ny;
    wrist->pz = nz;
    wrist->have_prev = true;

    bool refractory =
        wrist->have_event &&
        (uint32_t)(now_ms - wrist->last_event_ms) < cfg->refractory_ms;

    const float threshold = (float)cfg->angle_mdeg / 1000.0f;
    const uint32_t hold_ms = cfg->hold_ms ? cfg->hold_ms : MOSU_WRIST_HOLD_MS;
    const float face_up_min = cfg->face_up_milli
                                  ? (float)cfg->face_up_milli / 1000.0f
                                  : MOSU_WRIST_FACE_UP_MIN;
    const float from_baseline =
        angle_between(nx, ny, nz, wrist->bx, wrist->by, wrist->bz);

    if (!wrist->have_candidate) {
        if (!refractory && from_baseline >= threshold) {
            /* The wrist has turned far enough.  Remember the orientation and
             * start the hold timer; a raise that is still in progress will
             * keep replacing the candidate until the arm stops. */
            wrist->have_candidate = true;
            wrist->cx = nx;
            wrist->cy = ny;
            wrist->cz = nz;
            wrist->candidate_ms = now_ms;
            wrist->candidates++;
            return false;
        }

        /* Settled and close to the baseline: let the baseline follow slowly.
         * The settle delay and the distance limit keep a slow raise from being
         * absorbed while the arm is still turning. */
        if (!motion && wrist->still_ms >= MOSU_WRIST_SETTLE_MS &&
            from_baseline < MOSU_WRIST_TRACK_DEG) {
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

    const float from_candidate =
        angle_between(nx, ny, nz, wrist->cx, wrist->cy, wrist->cz);

    if (from_candidate <= MOSU_WRIST_HOLD_DEG) {
        const uint32_t held_ms = (uint32_t)(now_ms - wrist->candidate_ms);
        /* The arm has stopped near the candidate orientation.  The glass must
         * face the wearer: a reach across a table turns the wrist just as far
         * but leaves the screen normal horizontal. */
        if (held_ms >= hold_ms && !refractory && fabsf(nz) >= face_up_min) {
            wrist->last_angle_deg =
                angle_between(wrist->cx, wrist->cy, wrist->cz, wrist->bx,
                              wrist->by, wrist->bz);
            wrist->last_nz = nz;
            wrist->bx = wrist->cx;
            wrist->by = wrist->cy;
            wrist->bz = wrist->cz;
            wrist->have_candidate = false;
            wrist->last_event_ms = now_ms;
            wrist->have_event = true;
            wrist->still_ms = 0;
            return true;
        }

        /* This orientation is not a raise: the glass is vertical, or the
         * refractory window is still running.  Once it has held, take it as
         * the new resting orientation so the next raise is measured from the
         * arm position the wearer is actually in. */
        if (held_ms >= MOSU_WRIST_ADOPT_MS && !motion) {
            wrist->bx = wrist->cx;
            wrist->by = wrist->cy;
            wrist->bz = wrist->cz;
            wrist->have_candidate = false;
        }
        return false;
    }

    /* Still moving: restart the candidate at the current orientation. */
    wrist->cx = nx;
    wrist->cy = ny;
    wrist->cz = nz;
    wrist->candidate_ms = now_ms;
    return false;
}
