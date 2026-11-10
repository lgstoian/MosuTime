/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The BMP writer used by the hardware screenshot command.
 */
#include <stdint.h>
#include <string.h>

#include "mosu_core/mosu_bmp.h"
#include "test_util.h"

void test_bmp(void)
{
    EXPECT(mosu_bmp_stride(1) == 4);
    EXPECT(mosu_bmp_stride(2) == 8);
    EXPECT(mosu_bmp_stride(3) == 12);
    EXPECT(mosu_bmp_stride(410) == 1232);
    EXPECT(mosu_bmp_stride(411) == 1236);

    uint8_t header[MOSU_BMP_HEADER_SIZE];
    mosu_bmp_header(header, 410, 502);

    uint32_t stride = mosu_bmp_stride(410);
    uint32_t image = stride * 502u;

    EXPECT(header[0] == 'B' && header[1] == 'M');
    EXPECT(header[2] == (uint8_t)((54u + image) & 0xFF));
    EXPECT(header[10] == 54);
    EXPECT(header[14] == 40);
    EXPECT(header[18] == (uint8_t)(410 & 0xFF));
    EXPECT(header[19] == (uint8_t)(410 >> 8));
    EXPECT(header[22] == (uint8_t)(502 & 0xFF));
    EXPECT(header[23] == (uint8_t)(502 >> 8));
    EXPECT(header[26] == 1);
    EXPECT(header[28] == 24);
    EXPECT(header[30] == 0);
    EXPECT(header[34] == (uint8_t)(image & 0xFF));

    /* Red, green, blue in RGB565 become BGR in a BMP row. */
    uint16_t src[3] = {0xF800, 0x07E0, 0x001F};
    uint8_t row[12];
    memset(row, 0xAA, sizeof(row));
    mosu_bmp_row_rgb565(row, src, 3);

    EXPECT(row[0] == 0x00 && row[1] == 0x00 && row[2] == 0xFF);
    EXPECT(row[3] == 0x00 && row[4] == 0xFF && row[5] == 0x00);
    EXPECT(row[6] == 0xFF && row[7] == 0x00 && row[8] == 0x00);
    EXPECT(row[9] == 0 && row[10] == 0 && row[11] == 0);

    uint16_t white = 0xFFFF;
    uint16_t black = 0x0000;
    mosu_bmp_row_rgb565(row, &white, 1);
    EXPECT(row[0] == 0xFF && row[1] == 0xFF && row[2] == 0xFF);
    EXPECT(row[3] == 0); /* the one byte of row padding */
    mosu_bmp_row_rgb565(row, &black, 1);
    EXPECT(row[0] == 0 && row[1] == 0 && row[2] == 0);

    /* Mid-range values expand by bit replication, not by dropping bits. */
    uint16_t mid = 0x8410; /* r=16, g=32, b=16 */
    mosu_bmp_row_rgb565(row, &mid, 1);
    EXPECT(row[0] == 0x84 && row[1] == 0x82 && row[2] == 0x84);
}
