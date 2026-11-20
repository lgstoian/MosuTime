/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Pure C helpers shared by the firmware, the host tests and the desktop
 * preview.  Nothing in mosu_core may include ESP-IDF headers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOSU_MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MOSU_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MOSU_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- clock formatting -------------------------------------------------- */

/*
 * Split a 24 h clock value into the 12 h value shown on the watch.
 * 00:xx -> 12:xx AM, 12:xx PM stays 12.  The watch never shows AM/PM text;
 * the AM and PM colour themes carry that information instead.
 */
void mosu_time_split12(uint8_t hour24, uint8_t *hour12, bool *pm);

/*
 * Format the two stacked clock labels.
 *
 * hours   : 12 h, no leading zero, "1".."12"
 * minutes : always two digits, "00".."59"
 * both    : "--" when the time is not valid yet
 *
 * Buffers must be at least 3 bytes long.
 */
void mosu_format_clock(uint8_t hour24, uint8_t minute, bool valid,
                       char hours[3], char minutes[3]);

/* ---- announcement phrase ---------------------------------------------- */

typedef enum {
    MOSU_CLIP_HOUR = 0,
    MOSU_CLIP_MINUTE,
    MOSU_CLIP_DAY,
} mosu_clip_kind_t;

typedef struct {
    mosu_clip_kind_t kind;
    uint8_t index; /* hour 1..12, minute 1..59, weekday 0..6 (Mon..Sun) */
} mosu_clip_t;

#define MOSU_PHRASE_MAX_CLIPS 3

typedef struct {
    bool valid;
    uint8_t hour12;
    bool pm;
    uint8_t count;
    mosu_clip_t clips[MOSU_PHRASE_MAX_CLIPS];
} mosu_phrase_t;

/*
 * Build the ordered clip list for "Este ora <hour> si <minute> minute. <day>."
 *
 * weekday is 0 = Monday .. 6 = Sunday (see mosu_weekday_from_tm()).
 * Exact hours produce only the hour clip.  announce_day may be false.
 */
void mosu_phrase_build(uint8_t hour24, uint8_t minute, uint8_t weekday,
                       bool announce_day, mosu_phrase_t *out);

/* Convert struct tm.tm_wday (0 = Sunday) to the 0 = Monday form above. */
uint8_t mosu_weekday_from_tm(int tm_wday);

/*
 * Write the SD file name for a clip, e.g. "h_3.wav", "m_45.wav", "d_2.wav".
 * Returns false when index is out of range or the buffer is too small.
 */
bool mosu_clip_filename(mosu_clip_kind_t kind, uint8_t index, char *buf,
                        size_t len);

/* ---- battery ----------------------------------------------------------- */

typedef enum {
    MOSU_BATT_UNKNOWN = 0,
    MOSU_BATT_RED,
    MOSU_BATT_ORANGE,
    MOSU_BATT_GREEN,
} mosu_batt_bucket_t;

/* Single cell LiPo discharge curve, millivolts -> percent (0..100). */
uint8_t mosu_battery_percent(uint16_t millivolts);

/* Colour bucket for the tiny status icon. */
mosu_batt_bucket_t mosu_battery_bucket(uint8_t percent, uint8_t green_min,
                                       uint8_t orange_min);

/* ---- double tap -------------------------------------------------------- */

typedef struct {
    uint16_t double_tap_ms; /* max gap between the two taps */
    uint16_t max_hold_ms;   /* max duration of one tap */
    uint16_t max_move_px;   /* max finger travel of one tap */
    uint16_t cooldown_ms;   /* ignore input after a recognised double tap */
} mosu_tap_cfg_t;

typedef struct {
    bool pressed;
    int16_t x;
    int16_t y;
    uint32_t t_ms;
} mosu_touch_sample_t;

typedef struct {
    mosu_tap_cfg_t cfg;
    bool down;
    bool ignore;
    uint32_t down_ms;
    int16_t down_x;
    int16_t down_y;
    bool have_tap;
    uint32_t tap_ms;
    uint32_t cooldown_until;
} mosu_tap_t;

void mosu_tap_init(mosu_tap_t *tap, const mosu_tap_cfg_t *cfg);

/*
 * Feed one touch sample (polled at ~30 Hz).  Returns true exactly once for a
 * recognised double tap.  The caller never has to know about timing windows.
 */
bool mosu_tap_update(mosu_tap_t *tap, const mosu_touch_sample_t *sample);

/* ---- wrist raise ------------------------------------------------------- */

typedef struct {
    float x; /* g */
    float y;
    float z;
} mosu_accel_t;

typedef struct {
    uint16_t angle_mdeg;       /* orientation change that counts as a raise */
    uint16_t refractory_ms;
    /* Adaptive gates; 0 selects the built-in default. */
    uint16_t hold_ms;          /* how long the new orientation must hold */
    uint16_t face_up_milli;    /* |gz| * 1000 that counts as facing the wearer */
} mosu_wrist_cfg_t;

typedef struct {
    float bx, by, bz;      /* gravity baseline, unit vector */
    float cx, cy, cz;      /* candidate orientation, unit vector */
    float px, py, pz;      /* previous sample, unit vector */
    bool have_baseline;
    bool have_prev;
    bool have_candidate;
    bool have_event;
    bool moving;           /* between the motion onset and the next settled sample */
    uint32_t candidate_ms;
    uint32_t last_event_ms;
    uint32_t still_ms;     /* time since the last sample-to-sample movement */
    uint32_t last_ms;      /* previous sample time, 0 before the first */
    /* Diagnostics for the console and the learning policy. */
    float last_angle_deg;  /* size of the orientation change that fired */
    float last_nz;         /* screen-normal gravity component when it fired */
    uint16_t candidates;   /* raise-sized orientation changes seen */
} mosu_wrist_t;

void mosu_wrist_init(mosu_wrist_t *wrist);
void mosu_wrist_reset(mosu_wrist_t *wrist);

/*
 * Feed one accelerometer sample.  Returns true on the rising edge of a wrist
 * raise.
 *
 * The detector is deliberately built around the way an arm actually moves.
 * A raise is a large change of the gravity direction (the arm turns the glass
 * from vertical to facing the wearer) that then holds still for a moment.
 * The old version also required the arm to be still *at the sample where the
 * threshold was crossed*, which a continuous raise can never satisfy: the
 * threshold is crossed mid-movement, when the stillness timer has just been
 * reset.  Only a violent shake could jump the threshold in one sample.
 *
 * Stillness is still measured, but only to stop the slow baseline from
 * following the arm during a movement and to decide when a new resting
 * orientation has settled.
 */
bool mosu_wrist_update(mosu_wrist_t *wrist, const mosu_accel_t *accel,
                       const mosu_wrist_cfg_t *cfg, uint32_t now_ms);

/* ---- seven segment digits --------------------------------------------- */

/*
 * The clock numerals are drawn, not shaped from a font: nine giant digits do
 * not justify a megabyte font table, and drawn digits stay razor sharp at any
 * size.  The same code renders into LVGL on the watch and into a BMP on the
 * desktop preview.
 */
typedef struct {
    void *ctx;
    void (*line)(void *ctx, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                 uint8_t width, uint32_t rgb);
} mosu_canvas_t;

typedef struct {
    int16_t width;   /* digit box width */
    int16_t height;  /* digit box height */
    uint8_t stroke;  /* line thickness */
    uint32_t color;  /* 0xRRGGBB */
} mosu_digit_style_t;

/* Nominal width of one digit box for a given height. */
int16_t mosu_digit_width(int16_t height);

/*
 * Draw one digit.  x,y is the top-left of the digit box, digit is 0..9.
 * Any other value draws an empty box (used while the time is unknown).
 */
void mosu_digit_draw(const mosu_canvas_t *canvas, const mosu_digit_style_t *style,
                     int16_t x, int16_t y, uint8_t digit);

/*
 * Draw a string of digits (dashes allowed for "unknown").  Returns the total
 * width in pixels.
 */
int16_t mosu_digits_draw(const mosu_canvas_t *canvas,
                         const mosu_digit_style_t *style, int16_t x, int16_t y,
                         const char *text, int16_t gap);

/* Total width of a string when drawn with mosu_digits_draw(). */
int16_t mosu_digits_width(const mosu_digit_style_t *style, const char *text,
                          int16_t gap);

#ifdef __cplusplus
}
#endif
