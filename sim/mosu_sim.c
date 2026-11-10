/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Desktop preview / simulator.
 *
 * Renders the real watch face at the real resolution into BMP files, using the
 * same digit geometry and the same layout constants as the firmware.  No SDL,
 * no LVGL, no extra dependencies: it runs anywhere a C compiler exists, which
 * makes it usable in CI and by family members who just want to look at a
 * picture and say "yes, I can read that".
 *
 * Usage:
 *   mosu_sim [output-directory]
 *   mosu_sim --render <out.bmp> <hour24> <minute> <battery> <wifi 0|1|2> \
 *            <slash 0|1> <valid 0|1>
 *
 * The default mode writes the four preview scenes used by CI.  The --render
 * mode writes exactly one scene, which tools/contact_sheet.py uses to build
 * the accessibility contact sheet.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mosu_core/mosu_core.h"
#include "mosu_core/mosu_layout.h"

typedef struct {
    uint8_t *pixels; /* top-down RGB888 */
    int width;
    int height;
} image_t;

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void put_pixel(image_t *img, int x, int y, uint32_t color)
{
    if (x < 0 || y < 0 || x >= img->width || y >= img->height) {
        return;
    }
    uint8_t *p = img->pixels + ((size_t)y * img->width + x) * 3;
    p[0] = (uint8_t)(color >> 16);
    p[1] = (uint8_t)(color >> 8);
    p[2] = (uint8_t)color;
}

static void fill_rect(image_t *img, int x, int y, int w, int h, uint32_t color)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            put_pixel(img, xx, yy, color);
        }
    }
}

static float distance_to_segment(float px, float py, float x1, float y1,
                                 float x2, float y2)
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 0.0001f) {
        t = ((px - x1) * dx + (py - y1) * dy) / len2;
        if (t < 0.0f) {
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }
    }
    float cx = x1 + t * dx;
    float cy = y1 + t * dy;
    float ex = px - cx;
    float ey = py - cy;
    return sqrtf(ex * ex + ey * ey);
}

static void draw_line(void *ctx, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                      uint8_t width, uint32_t color)
{
    image_t *img = (image_t *)ctx;
    float radius = width / 2.0f;
    int min_x = (int)((x1 < x2 ? x1 : x2) - radius - 1);
    int max_x = (int)((x1 > x2 ? x1 : x2) + radius + 1);
    int min_y = (int)((y1 < y2 ? y1 : y2) - radius - 1);
    int max_y = (int)((y1 > y2 ? y1 : y2) + radius + 1);

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            float d = distance_to_segment((float)x + 0.5f, (float)y + 0.5f, x1,
                                          y1, x2, y2);
            if (d <= radius) {
                put_pixel(img, x, y, color);
            }
        }
    }
}

static void draw_circle(image_t *img, int cx, int cy, int radius, uint32_t color)
{
    for (int y = cy - radius; y <= cy + radius; y++) {
        for (int x = cx - radius; x <= cx + radius; x++) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius) {
                put_pixel(img, x, y, color);
            }
        }
    }
}

static void draw_battery(image_t *img, int x, int y, int percent, uint32_t color)
{
    int w = MOSU_BATTERY_W;
    int h = MOSU_BATTERY_H;
    int border = 3;

    /* Body outline. */
    fill_rect(img, x, y, w, border, color);
    fill_rect(img, x, y + h - border, w, border, color);
    fill_rect(img, x, y, border, h, color);
    fill_rect(img, x + w - border, y, border, h, color);

    /* Nub. */
    fill_rect(img, x + w, y + h / 3, 4, h / 3, color);

    /* Proportional fill: colour is never the only cue. */
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    int inner_w = (w - 2 * border - 4) * percent / 100;
    fill_rect(img, x + border + 2, y + border + 2, inner_w, h - 2 * border - 4,
              color);
}

static void draw_arc(image_t *img, int cx, int cy, int radius, int thickness,
                     float start_deg, float end_deg, uint32_t color)
{
    float r_in = (float)radius - thickness / 2.0f;
    float r_out = (float)radius + thickness / 2.0f;
    int box = radius + thickness;
    for (int y = cy - box; y <= cy + box; y++) {
        for (int x = cx - box; x <= cx + box; x++) {
            float dx = (float)(x - cx);
            float dy = (float)(y - cy);
            float d = sqrtf(dx * dx + dy * dy);
            if (d < r_in || d > r_out) {
                continue;
            }
            float angle = atan2f(dy, dx) * 57.2957795f;
            if (angle < 0.0f) {
                angle += 360.0f;
            }
            if (angle >= start_deg && angle <= end_deg) {
                put_pixel(img, x, y, color);
            }
        }
    }
}

static void draw_wifi(image_t *img, int x, int y, uint32_t color, bool slash)
{
    int cx = x + MOSU_WIFI_W / 2;
    int cy = y + MOSU_WIFI_H - 4;

    draw_circle(img, cx, cy, 3, color);
    draw_arc(img, cx, cy, 9, 3, 225.0f, 315.0f, color);
    draw_arc(img, cx, cy, 16, 3, 225.0f, 315.0f, color);
    draw_arc(img, cx, cy, 23, 3, 225.0f, 315.0f, color);

    if (slash) {
        draw_line(img, (int16_t)(x + 2), (int16_t)(y + MOSU_WIFI_H - 2),
                  (int16_t)(x + MOSU_WIFI_W - 2), (int16_t)(y + 2), 4, color);
    }
}

static void draw_clock_text(image_t *img, int y, const char *text,
                            mosu_digit_style_t *style, uint32_t color)
{
    style->color = color;
    int16_t total = mosu_digits_width(style, text, MOSU_DIGIT_GAP);
    int16_t x = (int16_t)((MOSU_LCD_W - total) / 2);

    mosu_canvas_t canvas = {
        .ctx = img,
        .line = draw_line,
    };
    mosu_digits_draw(&canvas, style, x, (int16_t)y, text, MOSU_DIGIT_GAP);
}

typedef enum {
    WIFI_OK = 0,
    WIFI_IDLE,
    WIFI_FAIL,
} wifi_state_t;

static void render_scene(image_t *img, bool pm, bool valid, uint8_t hour24,
                         uint8_t minute, int battery, wifi_state_t wifi,
                         bool slash)
{
    memset(img->pixels, 0, (size_t)img->width * img->height * 3);

    uint32_t time_color = pm ? rgb(0xFF, 0xC9, 0x8A) : rgb(0xFF, 0xFF, 0xFF);
    uint32_t batt_color = rgb(0x00, 0xE6, 0x76);
    if (battery < 15) {
        batt_color = rgb(0xFF, 0x17, 0x44);
    } else if (battery < 40) {
        batt_color = rgb(0xFF, 0xAB, 0x00);
    }

    uint32_t wifi_color = rgb(0x8A, 0x8A, 0x8A);
    if (wifi == WIFI_OK) {
        wifi_color = rgb(0x00, 0xE6, 0x76);
    } else if (wifi == WIFI_FAIL) {
        wifi_color = rgb(0xFF, 0x17, 0x44);
    }

    char hours[3];
    char minutes[3];
    mosu_format_clock(hour24, minute, valid, hours, minutes);

    mosu_digit_style_t style = {
        .width = mosu_digit_width(MOSU_CLOCK_H),
        .height = MOSU_CLOCK_H,
        .stroke = MOSU_CLOCK_STROKE,
    };

    draw_clock_text(img, MOSU_CLOCK_TOP, hours, &style, time_color);
    draw_clock_text(img, MOSU_CLOCK_Y_MINUTES, minutes, &style, time_color);

    draw_battery(img, MOSU_SCREEN_MARGIN, MOSU_STATUS_Y, battery, batt_color);
    draw_wifi(img, MOSU_LCD_W - MOSU_SCREEN_MARGIN - MOSU_WIFI_W, MOSU_STATUS_Y,
              wifi_color, slash && wifi == WIFI_FAIL);
}

static void write_bmp(const char *path, const image_t *img)
{
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }

    int row_size = (img->width * 3 + 3) & ~3;
    uint32_t pixel_bytes = (uint32_t)row_size * (uint32_t)img->height;
    uint32_t file_size = 54 + pixel_bytes;

    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (uint8_t)file_size;
    header[3] = (uint8_t)(file_size >> 8);
    header[4] = (uint8_t)(file_size >> 16);
    header[5] = (uint8_t)(file_size >> 24);
    header[10] = 54;
    header[14] = 40;
    header[18] = (uint8_t)img->width;
    header[19] = (uint8_t)(img->width >> 8);
    header[20] = (uint8_t)(img->width >> 16);
    header[21] = (uint8_t)(img->width >> 24);
    header[22] = (uint8_t)img->height;
    header[23] = (uint8_t)(img->height >> 8);
    header[24] = (uint8_t)(img->height >> 16);
    header[25] = (uint8_t)(img->height >> 24);
    header[26] = 1;
    header[28] = 24;
    header[34] = (uint8_t)pixel_bytes;
    header[35] = (uint8_t)(pixel_bytes >> 8);
    header[36] = (uint8_t)(pixel_bytes >> 16);
    header[37] = (uint8_t)(pixel_bytes >> 24);
    fwrite(header, 1, sizeof(header), file);

    uint8_t row_padding[3] = {0, 0, 0};
    for (int y = img->height - 1; y >= 0; y--) {
        for (int x = 0; x < img->width; x++) {
            const uint8_t *p = img->pixels + ((size_t)y * img->width + x) * 3;
            uint8_t bgr[3] = {p[2], p[1], p[0]};
            fwrite(bgr, 1, 3, file);
        }
        int padding = row_size - img->width * 3;
        if (padding > 0) {
            fwrite(row_padding, 1, (size_t)padding, file);
        }
    }
    fclose(file);
    printf("wrote %s\n", path);
}

static void print_usage(const char *argv0)
{
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s [output-directory]\n", argv0);
    fprintf(stderr,
            "  %s --render <out.bmp> <hour24> <minute> <battery> "
            "<wifi 0|1|2> <slash 0|1> <valid 0|1>\n",
            argv0);
}

static bool parse_int(const char *text, int min, int max, int *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);

    if (end == text || *end != '\0' || value < min || value > max) {
        return false;
    }
    *out = (int)value;
    return true;
}

/* One scene from explicit arguments: used by tools/contact_sheet.py. */
static int render_one(int argc, char **argv)
{
    if (argc != 9) {
        print_usage(argv[0]);
        return 2;
    }

    const char *out_path = argv[2];
    int hour24 = 0;
    int minute = 0;
    int battery = 0;
    int wifi = 0;
    int slash = 0;
    int valid = 0;

    if (!parse_int(argv[3], 0, 23, &hour24) ||
        !parse_int(argv[4], 0, 59, &minute) ||
        !parse_int(argv[5], 0, 100, &battery) ||
        !parse_int(argv[6], 0, 2, &wifi) ||
        !parse_int(argv[7], 0, 1, &slash) ||
        !parse_int(argv[8], 0, 1, &valid)) {
        fprintf(stderr, "mosu_sim: invalid scene argument\n");
        print_usage(argv[0]);
        return 2;
    }

    image_t img = {
        .pixels = malloc((size_t)MOSU_LCD_W * MOSU_LCD_H * 3),
        .width = MOSU_LCD_W,
        .height = MOSU_LCD_H,
    };
    if (!img.pixels) {
        return 1;
    }

    render_scene(&img, hour24 >= 12, valid != 0, (uint8_t)hour24,
                 (uint8_t)minute, battery, (wifi_state_t)wifi, slash != 0);
    write_bmp(out_path, &img);

    free(img.pixels);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--render") == 0) {
        return render_one(argc, argv);
    }

    const char *out_dir = argc > 1 ? argv[1] : "out";

    image_t img = {
        .pixels = malloc((size_t)MOSU_LCD_W * MOSU_LCD_H * 3),
        .width = MOSU_LCD_W,
        .height = MOSU_LCD_H,
    };
    if (!img.pixels) {
        return 1;
    }

    char path[512];

    snprintf(path, sizeof(path), "%s/01_am.bmp", out_dir);
    render_scene(&img, false, true, 9, 41, 78, WIFI_OK, false);
    write_bmp(path, &img);

    snprintf(path, sizeof(path), "%s/02_pm.bmp", out_dir);
    render_scene(&img, true, true, 21, 41, 32, WIFI_IDLE, false);
    write_bmp(path, &img);

    snprintf(path, sizeof(path), "%s/03_low_battery.bmp", out_dir);
    render_scene(&img, false, true, 7, 5, 9, WIFI_FAIL, true);
    write_bmp(path, &img);

    snprintf(path, sizeof(path), "%s/04_unknown.bmp", out_dir);
    render_scene(&img, false, false, 0, 0, 100, WIFI_IDLE, false);
    write_bmp(path, &img);

    free(img.pixels);
    return 0;
}
