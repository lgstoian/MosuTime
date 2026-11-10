/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "mosu_core/mosu_core.h"

void mosu_tap_init(mosu_tap_t *tap, const mosu_tap_cfg_t *cfg)
{
    if (!tap) {
        return;
    }
    memset(tap, 0, sizeof(*tap));
    if (cfg) {
        tap->cfg = *cfg;
    }
}

bool mosu_tap_update(mosu_tap_t *tap, const mosu_touch_sample_t *sample)
{
    if (!tap || !sample) {
        return false;
    }

    if (sample->pressed) {
        if (tap->down) {
            return false;
        }

        /* A new press outside the first tap window restarts the sequence. */
        if (tap->have_tap &&
            (int32_t)(sample->t_ms - tap->tap_ms) > (int32_t)tap->cfg.double_tap_ms) {
            tap->have_tap = false;
        }

        tap->down = true;
        tap->down_ms = sample->t_ms;
        tap->down_x = sample->x;
        tap->down_y = sample->y;
        tap->ignore = false;

        if (tap->cooldown_until != 0 &&
            (int32_t)(sample->t_ms - tap->cooldown_until) < 0) {
            tap->ignore = true;
        }
        return false;
    }

    /* Release edge. */
    if (!tap->down) {
        return false;
    }
    tap->down = false;

    if (tap->ignore) {
        return false;
    }

    uint32_t held = sample->t_ms - tap->down_ms;
    int32_t dx = (int32_t)sample->x - (int32_t)tap->down_x;
    int32_t dy = (int32_t)sample->y - (int32_t)tap->down_y;
    int32_t move2 = dx * dx + dy * dy;
    int32_t limit = (int32_t)tap->cfg.max_move_px;
    if (limit < 0) {
        limit = 0;
    }

    if (held > tap->cfg.max_hold_ms || move2 > limit * limit) {
        tap->have_tap = false;
        return false;
    }

    if (tap->have_tap) {
        tap->have_tap = false;
        tap->cooldown_until = sample->t_ms + tap->cfg.cooldown_ms;
        return true;
    }

    tap->have_tap = true;
    tap->tap_ms = sample->t_ms;
    return false;
}
