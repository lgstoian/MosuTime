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

/* Status row: small and secondary, but inside the AMOLED's rounded corners.
 * At 16/20 px the icons were clipped by the glass; 30/48 px keeps them in
 * the visible area on the real panel. */
#define MOSU_STATUS_Y 30
#define MOSU_STATUS_H 26
#define MOSU_SCREEN_MARGIN 48
#define MOSU_BATTERY_W 48
#define MOSU_BATTERY_H 24
#define MOSU_WIFI_W 34
#define MOSU_WIFI_H 28

/* Bring-up patterns use this inset so the corner squares are visible on
 * the real rounded panel instead of sitting in the cut-out.  28 px was
 * still clipped by the glass; 48 px matches the status margins. */
#define MOSU_SAFE_INSET 48

/* Two stacked clock lines: as large as the panel allows. */
#define MOSU_CLOCK_TOP 62
#define MOSU_CLOCK_H 200
#define MOSU_CLOCK_STROKE 22
#define MOSU_CLOCK_GAP 22
#define MOSU_DIGIT_GAP 16

#define MOSU_CLOCK_Y_MINUTES (MOSU_CLOCK_TOP + MOSU_CLOCK_H + MOSU_CLOCK_GAP)

/* Pixel-shift burn-in guard: the whole scene moves inside this box. */
#define MOSU_SHIFT_PX 2
