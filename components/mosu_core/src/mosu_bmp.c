/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "mosu_core/mosu_bmp.h"

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

uint32_t mosu_bmp_stride(uint16_t width)
{
    return ((uint32_t)width * 3u + 3u) & ~3u;
}

void mosu_bmp_header(uint8_t out[MOSU_BMP_HEADER_SIZE], uint16_t width,
                     uint16_t height)
{
    if (!out) {
        return;
    }

    uint32_t stride = mosu_bmp_stride(width);
    uint32_t image_size = stride * height;

    memset(out, 0, MOSU_BMP_HEADER_SIZE);

    /* BITMAPFILEHEADER */
    out[0] = 'B';
    out[1] = 'M';
    put_u32(out + 2, MOSU_BMP_HEADER_SIZE + image_size);
    put_u32(out + 10, MOSU_BMP_HEADER_SIZE);

    /* BITMAPINFOHEADER */
    put_u32(out + 14, 40);
    put_u32(out + 18, width);
    put_u32(out + 22, height); /* positive = bottom-up */
    put_u16(out + 26, 1);      /* planes */
    put_u16(out + 28, 24);     /* bits per pixel */
    put_u32(out + 30, 0);      /* BI_RGB */
    put_u32(out + 34, image_size);
    put_u32(out + 38, 2835); /* 72 dpi */
    put_u32(out + 42, 2835);
}

void mosu_bmp_row_rgb565(uint8_t *dst, const uint16_t *src, uint16_t width)
{
    if (!dst || !src) {
        return;
    }

    for (uint16_t x = 0; x < width; x++) {
        uint16_t value = src[x];
        uint8_t r5 = (uint8_t)((value >> 11) & 0x1F);
        uint8_t g6 = (uint8_t)((value >> 5) & 0x3F);
        uint8_t b5 = (uint8_t)(value & 0x1F);

        /* Bit replication is exact at both ends: 31 -> 255, 63 -> 255. */
        uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
        uint8_t g8 = (uint8_t)((g6 << 2) | (g6 >> 4));
        uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));

        dst[x * 3 + 0] = b8;
        dst[x * 3 + 1] = g8;
        dst[x * 3 + 2] = r8;
    }

    uint32_t stride = mosu_bmp_stride(width);
    uint32_t used = (uint32_t)width * 3u;
    if (stride > used) {
        memset(dst + used, 0, stride - used);
    }
}
