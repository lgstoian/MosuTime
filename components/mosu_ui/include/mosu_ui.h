/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The whole watch face is one custom-drawn LVGL screen: no fonts, no images,
 * no widgets.  That keeps the flash footprint small and means a full redraw
 * costs exactly the pixels the numerals need.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOSU_UI_WIFI_IDLE = 0, /* grey: no sync attempted yet */
    MOSU_UI_WIFI_OK,       /* green: last sync succeeded */
    MOSU_UI_WIFI_FAIL,     /* red: last sync failed */
} mosu_ui_wifi_t;

esp_err_t mosu_ui_create(const mosu_config_t *cfg);

void mosu_ui_set_time(uint8_t hour24, uint8_t minute, bool valid);
void mosu_ui_set_battery(uint8_t percent, bool present);
void mosu_ui_set_wifi(mosu_ui_wifi_t state);

/* Brief coloured border so a tap has visible feedback even in silence. */
void mosu_ui_flash_tap(void);

/*
 * Raw touch edges, for the double-tap detector.  Called from the LVGL task.
 */
typedef void (*mosu_ui_touch_cb_t)(bool pressed, int16_t x, int16_t y,
                                   void *user);
void mosu_ui_set_touch_cb(mosu_ui_touch_cb_t cb, void *user);

/* Force a full repaint; used every time the panel is switched back on. */
void mosu_ui_refresh(void);

#ifdef __cplusplus
}
#endif
