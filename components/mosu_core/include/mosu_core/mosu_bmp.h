/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Tiny 24-bit BMP writer.  The watch can capture its LVGL screen to the SD
 * card for documentation and for a first-hardware visual check; this is the
 * pure part, so the header and the RGB565 conversion are host-tested.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOSU_BMP_HEADER_SIZE 54u

/* Row pitch in bytes, padded to a 4-byte boundary as BMP requires. */
uint32_t mosu_bmp_stride(uint16_t width);

/*
 * Fill a standard 14 + 40 byte BMP header for a bottom-up 24-bit image.
 * height is positive: the rows are written bottom-up, as BMP expects.
 */
void mosu_bmp_header(uint8_t out[MOSU_BMP_HEADER_SIZE], uint16_t width,
                     uint16_t height);

/*
 * Convert one RGB565 row to the BMP's BGR888 byte order and zero the row
 * padding.  dst must hold mosu_bmp_stride(width) bytes.
 */
void mosu_bmp_row_rgb565(uint8_t *dst, const uint16_t *src, uint16_t width);

#ifdef __cplusplus
}
#endif
