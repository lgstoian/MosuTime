/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>

#include "mosu_core/mosu_core.h"

/* Segment bits, clockwise from the top: a b c d e f g. */
enum {
    SEG_A = 1u << 0,
    SEG_B = 1u << 1,
    SEG_C = 1u << 2,
    SEG_D = 1u << 3,
    SEG_E = 1u << 4,
    SEG_F = 1u << 5,
    SEG_G = 1u << 6,
};

static const uint8_t s_digits[10] = {
    0x3F, /* 0 */
    0x06, /* 1 */
    0x5B, /* 2 */
    0x4F, /* 3 */
    0x66, /* 4 */
    0x6D, /* 5 */
    0x7D, /* 6 */
    0x07, /* 7 */
    0x7F, /* 8 */
    0x6F, /* 9 */
};

int16_t mosu_digit_width(int16_t height)
{
    if (height <= 0) {
        return 0;
    }
    int16_t width = (int16_t)(((int32_t)height * 58) / 100);
    if (width < 10) {
        width = 10;
    }
    return width;
}

void mosu_digit_draw(const mosu_canvas_t *canvas, const mosu_digit_style_t *style,
                     int16_t x, int16_t y, uint8_t digit)
{
    if (!canvas || !canvas->line || !style || digit > 9) {
        return;
    }

    int16_t w = style->width;
    int16_t h = style->height;
    int16_t t = style->stroke < 2 ? 2 : style->stroke;
    if (w <= t * 2 || h <= t * 2) {
        return;
    }

    int16_t inset = (int16_t)(t / 2 + 1);
    int16_t x0 = (int16_t)(x + inset);
    int16_t x1 = (int16_t)(x + w - inset);
    int16_t y0 = (int16_t)(y + inset);
    int16_t y1 = (int16_t)(y + h / 2);
    int16_t y2 = (int16_t)(y + h - inset);
    int16_t half = (int16_t)(t / 2);

    int16_t xa = (int16_t)(x0 + half);
    int16_t xb = (int16_t)(x1 - half);
    int16_t ya = (int16_t)(y0 + half);
    int16_t yb = (int16_t)(y2 - half);

    uint8_t mask = s_digits[digit];

    /* A lone right-hand bar is hard to read as "1" on a mostly empty face.
     * Draw it as one bold, centred stem instead. */
    if (digit == 1) {
        int16_t cx = (int16_t)(x + w / 2);
        canvas->line(canvas->ctx, cx, y0, cx, y2, (uint8_t)t, style->color);
        return;
    }

    if (mask & SEG_A) {
        canvas->line(canvas->ctx, xa, y0, xb, y0, (uint8_t)t, style->color);
    }
    if (mask & SEG_B) {
        canvas->line(canvas->ctx, x1, ya, x1, y1, (uint8_t)t, style->color);
    }
    if (mask & SEG_C) {
        canvas->line(canvas->ctx, x1, y1, x1, yb, (uint8_t)t, style->color);
    }
    if (mask & SEG_D) {
        canvas->line(canvas->ctx, xa, y2, xb, y2, (uint8_t)t, style->color);
    }
    if (mask & SEG_E) {
        canvas->line(canvas->ctx, x0, y1, x0, yb, (uint8_t)t, style->color);
    }
    if (mask & SEG_F) {
        canvas->line(canvas->ctx, x0, ya, x0, y1, (uint8_t)t, style->color);
    }
    if (mask & SEG_G) {
        canvas->line(canvas->ctx, xa, y1, xb, y1, (uint8_t)t, style->color);
    }
}

int16_t mosu_digits_draw(const mosu_canvas_t *canvas,
                         const mosu_digit_style_t *style, int16_t x, int16_t y,
                         const char *text, int16_t gap)
{
    if (!canvas || !style || !text) {
        return 0;
    }

    int16_t cursor = x;
    for (const char *p = text; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            mosu_digit_draw(canvas, style, cursor, y, (uint8_t)(*p - '0'));
        }
        cursor = (int16_t)(cursor + style->width + gap);
    }

    int16_t total = mosu_digits_width(style, text, gap);
    (void)cursor;
    return total;
}

int16_t mosu_digits_width(const mosu_digit_style_t *style, const char *text,
                          int16_t gap)
{
    if (!style || !text) {
        return 0;
    }

    int count = 0;
    for (const char *p = text; *p; p++) {
        count++;
    }
    if (count == 0) {
        return 0;
    }
    return (int16_t)(count * style->width + (count - 1) * gap);
}
