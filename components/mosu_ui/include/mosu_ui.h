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

/* Bring-up patterns that replace the clock while the panel is being checked. */
typedef enum {
    MOSU_UI_TEST_NONE = 0, /* normal watch face */
    MOSU_UI_TEST_COLORS,   /* red/green/blue/white/grey bands */
    MOSU_UI_TEST_GRID,     /* white grid, 1 px every 50 px */
    MOSU_UI_TEST_CORNERS,  /* corner squares and a centre cross */
} mosu_ui_test_t;

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

/*
 * The display manager has exactly two scenes, and the wearer only ever sees
 * the clock.  BLANK is for maintenance: panel lit, black glass, no digits.
 */
typedef enum {
    MOSU_UI_SCENE_CLOCK = 0,
    MOSU_UI_SCENE_BLANK,
} mosu_ui_scene_t;

void mosu_ui_set_scene(mosu_ui_scene_t scene);
mosu_ui_scene_t mosu_ui_scene(void);

/*
 * Draw a bring-up pattern instead of the clock.  The patterns are absolute:
 * the burn-in pixel shift is ignored so the edges of the glass and the panel
 * offset can be judged by eye.  MOSU_UI_TEST_NONE brings the face back.
 */
void mosu_ui_test_pattern(mosu_ui_test_t pattern);

/*
 * Capture the current LVGL scene as a 24-bit BMP on the SD card.  On hardware
 * this is the only way to see exactly what the watch draws, independent of
 * the panel's QSPI stream.  The call allocates the RGB565 snapshot in PSRAM
 * and releases it before returning.
 */
esp_err_t mosu_ui_snapshot(const char *path);

#ifdef __cplusplus
}
#endif
