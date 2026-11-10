/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"

#include "mosu_core/mosu_bmp.h"
#include "mosu_core/mosu_core.h"
#include "mosu_core/mosu_layout.h"
#include "mosu_ui.h"

static const char *TAG = "mosu_ui";

typedef struct {
    lv_layer_t *layer;
    int16_t dx;
    int16_t dy;
} draw_ctx_t;

static const mosu_config_t *s_cfg;

static uint8_t s_hour24;
static uint8_t s_minute;
static bool s_valid;
static uint8_t s_battery;
static bool s_battery_present;
static mosu_ui_wifi_t s_wifi;

static int8_t s_dx;
static int8_t s_dy;
static uint8_t s_shift_step;

static uint32_t s_tap_until;
static uint32_t s_tap_ms;

static mosu_ui_test_t s_test;

static mosu_ui_touch_cb_t s_touch_cb;
static void *s_touch_user;

static lv_obj_t *s_screen;

/* ------------------------------------------------------------------ lines */

static void canvas_line(void *ctx, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                        uint8_t width, uint32_t rgb)
{
    draw_ctx_t *c = (draw_ctx_t *)ctx;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1.x = (float)(x1 + c->dx);
    dsc.p1.y = (float)(y1 + c->dy);
    dsc.p2.x = (float)(x2 + c->dx);
    dsc.p2.y = (float)(y2 + c->dy);
    dsc.width = width;
    dsc.color = lv_color_hex(rgb);
    dsc.round_start = 1;
    dsc.round_end = 1;
    lv_draw_line(c->layer, &dsc);
}

static void fill_rect(lv_layer_t *layer, int32_t x, int32_t y, int32_t w,
                      int32_t h, int32_t radius, uint32_t rgb,
                      lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(rgb);
    dsc.bg_opa = opa;
    dsc.radius = radius;

    lv_area_t area = {
        .x1 = (lv_coord_t)x,
        .y1 = (lv_coord_t)y,
        .x2 = (lv_coord_t)(x + w - 1),
        .y2 = (lv_coord_t)(y + h - 1),
    };
    lv_draw_rect(layer, &dsc, &area);
}

static void stroke_rect(lv_layer_t *layer, int32_t x, int32_t y, int32_t w,
                        int32_t h, int32_t radius, int32_t border,
                        uint32_t rgb, lv_opa_t opa)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = lv_color_hex(rgb);
    dsc.border_opa = opa;
    dsc.border_width = border;
    dsc.radius = radius;

    lv_area_t area = {
        .x1 = (lv_coord_t)x,
        .y1 = (lv_coord_t)y,
        .x2 = (lv_coord_t)(x + w - 1),
        .y2 = (lv_coord_t)(y + h - 1),
    };
    lv_draw_rect(layer, &dsc, &area);
}

static void stroke_arc(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t radius,
                       int32_t width, int32_t start, int32_t end, uint32_t rgb)
{
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.center.x = (lv_coord_t)cx;
    dsc.center.y = (lv_coord_t)cy;
    dsc.radius = radius;
    dsc.width = width;
    dsc.start_angle = start;
    dsc.end_angle = end;
    dsc.color = lv_color_hex(rgb);
    dsc.opa = LV_OPA_COVER;
    dsc.rounded = 1;
    lv_draw_arc(layer, &dsc);
}

/* ---------------------------------------------------------- test patterns */

/*
 * The patterns are drawn without the burn-in offset on purpose: they are a
 * ruler for the glass, so a moving ruler would defeat the measurement.
 */

static void test_line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2,
                      int32_t y2, int32_t width, uint32_t rgb)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.p1.x = (float)x1;
    dsc.p1.y = (float)y1;
    dsc.p2.x = (float)x2;
    dsc.p2.y = (float)y2;
    dsc.width = width;
    dsc.color = lv_color_hex(rgb);
    /* Square ends: a rounded cap would make a 1 px line look 2 px at the
     * edge of the glass. */
    dsc.round_start = 0;
    dsc.round_end = 0;
    lv_draw_line(layer, &dsc);
}

static void draw_test_colors(lv_layer_t *layer)
{
    static const uint32_t bands[] = {
        0xFF0000, /* red */
        0x00FF00, /* green */
        0x0000FF, /* blue */
        0xFFFFFF, /* white */
        0x808080, /* grey */
    };
    const int32_t count = (int32_t)(sizeof(bands) / sizeof(bands[0]));

    for (int32_t i = 0; i < count; i++) {
        int32_t top = (MOSU_LCD_H * i) / count;
        int32_t bottom = (MOSU_LCD_H * (i + 1)) / count;
        fill_rect(layer, 0, top, MOSU_LCD_W, bottom - top, 0, bands[i],
                  LV_OPA_COVER);
    }
}

static void draw_test_grid(lv_layer_t *layer)
{
    fill_rect(layer, 0, 0, MOSU_LCD_W, MOSU_LCD_H, 0, 0x000000, LV_OPA_COVER);

    for (int32_t x = 0; x < MOSU_LCD_W; x += 50) {
        test_line(layer, x, 0, x, MOSU_LCD_H - 1, 1, 0xFFFFFF);
    }
    for (int32_t y = 0; y < MOSU_LCD_H; y += 50) {
        test_line(layer, 0, y, MOSU_LCD_W - 1, y, 1, 0xFFFFFF);
    }

    /* The panel is 410 x 502: the last multiple of 50 is not the edge, so
     * the border needs its own two lines. */
    test_line(layer, MOSU_LCD_W - 1, 0, MOSU_LCD_W - 1, MOSU_LCD_H - 1, 1,
              0xFFFFFF);
    test_line(layer, 0, MOSU_LCD_H - 1, MOSU_LCD_W - 1, MOSU_LCD_H - 1, 1,
              0xFFFFFF);
}

static void draw_test_corners(lv_layer_t *layer)
{
    const int32_t m = 20;

    fill_rect(layer, 0, 0, MOSU_LCD_W, MOSU_LCD_H, 0, 0x000000, LV_OPA_COVER);
    fill_rect(layer, 0, 0, m, m, 0, 0xFFFFFF, LV_OPA_COVER);
    fill_rect(layer, MOSU_LCD_W - m, 0, m, m, 0, 0xFFFFFF, LV_OPA_COVER);
    fill_rect(layer, 0, MOSU_LCD_H - m, m, m, 0, 0xFFFFFF, LV_OPA_COVER);
    fill_rect(layer, MOSU_LCD_W - m, MOSU_LCD_H - m, m, m, 0, 0xFFFFFF,
              LV_OPA_COVER);

    /* The cross must sit in the middle of the visible area, not 22 px to one
     * side: that is exactly the X gap the panel driver compensates for. */
    const int32_t cx = MOSU_LCD_W / 2;
    const int32_t cy = MOSU_LCD_H / 2;
    test_line(layer, cx - 30, cy, cx + 30, cy, 2, 0xFFFFFF);
    test_line(layer, cx, cy - 30, cx, cy + 30, 2, 0xFFFFFF);
}

static void draw_test_pattern(lv_layer_t *layer, mosu_ui_test_t pattern)
{
    switch (pattern) {
    case MOSU_UI_TEST_COLORS:
        draw_test_colors(layer);
        break;
    case MOSU_UI_TEST_GRID:
        draw_test_grid(layer);
        break;
    case MOSU_UI_TEST_CORNERS:
        draw_test_corners(layer);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- numerals */

static void draw_clock_line(lv_layer_t *layer, int16_t top, const char *text,
                            uint32_t color)
{
    mosu_digit_style_t style = {
        .width = mosu_digit_width(MOSU_CLOCK_H),
        .height = MOSU_CLOCK_H,
        .stroke = MOSU_CLOCK_STROKE,
        .color = color,
    };

    draw_ctx_t ctx = {.layer = layer, .dx = s_dx, .dy = s_dy};
    mosu_canvas_t canvas = {.ctx = &ctx, .line = canvas_line};

    int16_t total = mosu_digits_width(&style, text, MOSU_DIGIT_GAP);
    int16_t x = (int16_t)((MOSU_LCD_W - total) / 2);
    mosu_digits_draw(&canvas, &style, x, top, text, MOSU_DIGIT_GAP);
}

/* ------------------------------------------------------------------ status */

static void draw_battery(lv_layer_t *layer)
{
    if (!s_cfg->status.show_battery) {
        return;
    }

    int32_t x = MOSU_SCREEN_MARGIN + s_dx;
    int32_t y = MOSU_STATUS_Y + s_dy;
    int32_t w = MOSU_BATTERY_W;
    int32_t h = MOSU_BATTERY_H;

    uint32_t color = s_cfg->colors.battery_green;
    if (!s_battery_present) {
        color = s_cfg->colors.wifi_idle;
    } else {
        switch (mosu_battery_bucket(s_battery, s_cfg->status.battery_green_min,
                                    s_cfg->status.battery_orange_min)) {
        case MOSU_BATT_RED:
            color = s_cfg->colors.battery_red;
            break;
        case MOSU_BATT_ORANGE:
            color = s_cfg->colors.battery_orange;
            break;
        default:
            color = s_cfg->colors.battery_green;
            break;
        }
    }

    /* The shell is always drawn, the fill shows the level: colour is never
     * the only cue. */
    stroke_rect(layer, x, y, w, h, 4, 3, color, LV_OPA_COVER);
    fill_rect(layer, x + w, y + h / 3, 4, h / 3, 1, color, LV_OPA_COVER);

    uint8_t percent = s_battery_present ? s_battery : 0;
    int32_t inner_max = w - 12;
    int32_t inner = (inner_max * percent) / 100;
    if (inner > 0) {
        fill_rect(layer, x + 6, y + 6, inner, h - 12, 2, color, LV_OPA_COVER);
    }
}

static void draw_wifi(lv_layer_t *layer)
{
    if (!s_cfg->status.show_wifi) {
        return;
    }

    int32_t x = MOSU_LCD_W - MOSU_SCREEN_MARGIN - MOSU_WIFI_W + s_dx;
    int32_t y = MOSU_STATUS_Y + s_dy;
    int32_t cx = x + MOSU_WIFI_W / 2;
    int32_t cy = y + MOSU_WIFI_H - 4;

    uint32_t color = s_cfg->colors.wifi_idle;
    if (s_wifi == MOSU_UI_WIFI_OK) {
        color = s_cfg->colors.wifi_ok;
    } else if (s_wifi == MOSU_UI_WIFI_FAIL) {
        color = s_cfg->colors.wifi_fail;
    }

    fill_rect(layer, cx - 3, cy - 3, 6, 6, 3, color, LV_OPA_COVER);
    stroke_arc(layer, cx, cy, 9, 3, 225, 315, color);
    stroke_arc(layer, cx, cy, 16, 3, 225, 315, color);
    stroke_arc(layer, cx, cy, 23, 3, 225, 315, color);

    if (s_cfg->status.wifi_fail_slash && s_wifi == MOSU_UI_WIFI_FAIL) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.p1.x = (float)(x + 2);
        dsc.p1.y = (float)(y + MOSU_WIFI_H - 2);
        dsc.p2.x = (float)(x + MOSU_WIFI_W - 2);
        dsc.p2.y = (float)(y + 2);
        dsc.width = 4;
        dsc.color = lv_color_hex(color);
        dsc.round_start = 1;
        dsc.round_end = 1;
        lv_draw_line(layer, &dsc);
    }
}

static void draw_tap_border(lv_layer_t *layer)
{
    uint32_t now = lv_tick_get();
    if (s_tap_until == 0 || (int32_t)(now - s_tap_until) >= 0) {
        return;
    }

    uint32_t remaining = s_tap_until - now;
    lv_opa_t opa = (lv_opa_t)((remaining * 255u) / (s_tap_ms ? s_tap_ms : 1));

    stroke_rect(layer, 4 + s_dx, 4 + s_dy, MOSU_LCD_W - 8, MOSU_LCD_H - 8, 12, 4,
                s_cfg->colors.tap_border, opa);
}

/* -------------------------------------------------------------- draw event */

static void draw_event_cb(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    if (!layer || !s_cfg) {
        return;
    }

    if (s_test != MOSU_UI_TEST_NONE) {
        draw_test_pattern(layer, s_test);
        return;
    }

    uint8_t hour12;
    bool pm;
    mosu_time_split12(s_hour24, &hour12, &pm);

    uint32_t time_color =
        pm ? s_cfg->colors.time_pm : s_cfg->colors.time_am;

    char hours[3];
    char minutes[3];
    mosu_format_clock(s_hour24, s_minute, s_valid, hours, minutes);

    draw_clock_line(layer, MOSU_CLOCK_TOP, hours, time_color);
    draw_clock_line(layer, MOSU_CLOCK_Y_MINUTES, minutes, time_color);

    draw_battery(layer);
    draw_wifi(layer);
    draw_tap_border(layer);
}

/* ------------------------------------------------------------------ timers */

static void touch_event_cb(lv_event_t *event)
{
    if (!s_touch_cb) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED) {
        return;
    }

    lv_point_t point = {0, 0};
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &point);
    }

    s_touch_cb(code == LV_EVENT_PRESSED, (int16_t)point.x, (int16_t)point.y,
               s_touch_user);
}

static void invalidate(void)
{
    if (s_screen) {
        lv_obj_invalidate(s_screen);
    }
}

static void housekeeping_timer(lv_timer_t *timer)
{
    (void)timer;

    /* Keep the tap border animating while it is fading out. */
    if (s_tap_until && (int32_t)(lv_tick_get() - s_tap_until) < 0) {
        invalidate();
    }
}

static void shift_timer(lv_timer_t *timer)
{
    (void)timer;

    /* A test pattern is a ruler: moving it would make the measurement
     * meaningless, so skip the repaint while one is shown. */
    if (s_test != MOSU_UI_TEST_NONE) {
        return;
    }

    s_shift_step = (uint8_t)((s_shift_step + 1) % 9);
    s_dx = (int8_t)(((int)s_shift_step / 3 - 1) * MOSU_SHIFT_PX);
    s_dy = (int8_t)(((int)s_shift_step % 3 - 1) * MOSU_SHIFT_PX);
    invalidate();
}

/* --------------------------------------------------------------- public API */

esp_err_t mosu_ui_create(const mosu_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;
    s_battery_present = false;

    if (!lvgl_port_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    s_screen = lv_screen_active();
    lv_obj_clean(s_screen);
    lv_obj_set_scrollable(s_screen, false);
    lv_obj_set_clickable(s_screen, true);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(cfg->colors.background), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);

    lv_obj_add_event_cb(s_screen, draw_event_cb, LV_EVENT_DRAW_POST, NULL);
    lv_obj_add_event_cb(s_screen, touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_screen, touch_event_cb, LV_EVENT_RELEASED, NULL);

    lv_timer_create(housekeeping_timer, 33, NULL);

    if (cfg->display.pixel_shift_sec > 0) {
        lv_timer_create(shift_timer, (uint32_t)cfg->display.pixel_shift_sec * 1000u,
                        NULL);
    }

    lv_obj_invalidate(s_screen);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "watch face ready");
    return ESP_OK;
}

void mosu_ui_set_time(uint8_t hour24, uint8_t minute, bool valid)
{
    if (!s_screen) {
        return;
    }
    if (s_hour24 == hour24 && s_minute == minute && s_valid == valid) {
        return;
    }
    s_hour24 = hour24;
    s_minute = minute;
    s_valid = valid;

    if (lvgl_port_lock(1000)) {
        invalidate();
        lvgl_port_unlock();
    }
}

void mosu_ui_set_battery(uint8_t percent, bool present)
{
    if (!s_screen) {
        return;
    }
    if (s_battery == percent && s_battery_present == present) {
        return;
    }
    s_battery = percent;
    s_battery_present = present;

    if (lvgl_port_lock(1000)) {
        invalidate();
        lvgl_port_unlock();
    }
}

void mosu_ui_set_wifi(mosu_ui_wifi_t state)
{
    if (!s_screen) {
        return;
    }
    if (s_wifi == state) {
        return;
    }
    s_wifi = state;

    if (lvgl_port_lock(1000)) {
        invalidate();
        lvgl_port_unlock();
    }
}

void mosu_ui_set_touch_cb(mosu_ui_touch_cb_t cb, void *user)
{
    s_touch_user = user;
    s_touch_cb = cb;
}

void mosu_ui_refresh(void)
{
    if (!s_screen) {
        return;
    }
    if (lvgl_port_lock(1000)) {
        invalidate();
        lvgl_port_unlock();
    }
}

void mosu_ui_flash_tap(void)
{
    if (!s_cfg || !s_screen) {
        return;
    }
    s_tap_ms = s_cfg->display.tap_feedback_ms;
    if (s_tap_ms == 0) {
        return;
    }

    if (lvgl_port_lock(1000)) {
        s_tap_until = lv_tick_get() + s_tap_ms;
        invalidate();
        lvgl_port_unlock();
    }
}

void mosu_ui_test_pattern(mosu_ui_test_t pattern)
{
    switch (pattern) {
    case MOSU_UI_TEST_NONE:
    case MOSU_UI_TEST_COLORS:
    case MOSU_UI_TEST_GRID:
    case MOSU_UI_TEST_CORNERS:
        break;
    default:
        /* An unknown value from the console must not leave the face stuck
         * on a pattern that nobody knows how to name. */
        pattern = MOSU_UI_TEST_NONE;
        break;
    }

    s_test = pattern;

    if (!s_screen) {
        return;
    }
    if (lvgl_port_lock(1000)) {
        invalidate();
        lvgl_port_unlock();
    }
}

esp_err_t mosu_ui_snapshot(const char *path)
{
#if !CONFIG_LV_USE_SNAPSHOT
    (void)path;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!path || !s_screen) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t width = MOSU_LCD_W;
    const uint16_t height = MOSU_LCD_H;
    const size_t pixels_size = (size_t)width * height * 2u;

    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t *pixels =
        heap_caps_malloc(pixels_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) {
        pixels = heap_caps_malloc(pixels_size, MALLOC_CAP_DEFAULT);
    }
    lv_draw_buf_t *draw_buf =
        pixels ? lv_malloc(sizeof(lv_draw_buf_t)) : NULL;
    if (!pixels || !draw_buf) {
        lvgl_port_unlock();
        heap_caps_free(pixels);
        lv_free(draw_buf);
        return ESP_ERR_NO_MEM;
    }

    memset(draw_buf, 0, sizeof(*draw_buf));
    lv_result_t result = lv_draw_buf_init(draw_buf, width, height,
                                          LV_COLOR_FORMAT_RGB565, width * 2,
                                          pixels, pixels_size);
    if (result == LV_RESULT_OK) {
        result = lv_snapshot_take_to_draw_buf(s_screen,
                                              LV_COLOR_FORMAT_RGB565, draw_buf);
    }
    lvgl_port_unlock();

    if (result != LV_RESULT_OK) {
        lv_free(draw_buf);
        heap_caps_free(pixels);
        ESP_LOGE(TAG, "snapshot failed");
        return ESP_FAIL;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        lv_free(draw_buf);
        heap_caps_free(pixels);
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_FAIL;
    }

    uint8_t header[MOSU_BMP_HEADER_SIZE];
    mosu_bmp_header(header, width, height);
    const uint32_t stride = mosu_bmp_stride(width);
    uint8_t *row = heap_caps_malloc(stride, MALLOC_CAP_DEFAULT);
    bool ok = row != NULL && fwrite(header, 1, sizeof(header), file) ==
                                  sizeof(header);

    for (int y = (int)height - 1; y >= 0 && ok; y--) {
        const uint16_t *src =
            (const uint16_t *)(pixels + (size_t)y * width * 2u);
        mosu_bmp_row_rgb565(row, src, width);
        if (fwrite(row, 1, stride, file) != stride) {
            ok = false;
        }
    }

    fclose(file);
    heap_caps_free(row);
    lv_free(draw_buf);
    heap_caps_free(pixels);

    if (!ok) {
        ESP_LOGE(TAG, "failed to write %s", path);
        remove(path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "snapshot written to %s (%dx%d)", path, width, height);
    return ESP_OK;
#endif
}
