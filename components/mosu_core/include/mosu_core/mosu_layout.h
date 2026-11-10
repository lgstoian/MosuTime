/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Shared screen layout.  The firmware UI and the desktop preview both use
 * these numbers, so what the family approves on a PC is what the watch shows.
 */
#pragma once

#define MOSU_LCD_W 410
#define MOSU_LCD_H 502

/* Status row: small, in the corners, never competing with the numerals. */
#define MOSU_STATUS_Y 16
#define MOSU_STATUS_H 26
#define MOSU_SCREEN_MARGIN 20
#define MOSU_BATTERY_W 48
#define MOSU_BATTERY_H 24
#define MOSU_WIFI_W 34
#define MOSU_WIFI_H 28

/* Two stacked clock lines: as large as the panel allows. */
#define MOSU_CLOCK_TOP 62
#define MOSU_CLOCK_H 200
#define MOSU_CLOCK_STROKE 22
#define MOSU_CLOCK_GAP 22
#define MOSU_DIGIT_GAP 16

#define MOSU_CLOCK_Y_MINUTES (MOSU_CLOCK_TOP + MOSU_CLOCK_H + MOSU_CLOCK_GAP)

/* Pixel-shift burn-in guard: the whole scene moves inside this box. */
#define MOSU_SHIFT_PX 2
