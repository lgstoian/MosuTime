/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The only component that knows about this particular board.
 * A future port replaces mosu_board and nothing else.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pins (Waveshare ESP32-S3-Touch-AMOLED-2.06) ----------------------- */
#define MOSU_PIN_I2C_SCL 14
#define MOSU_PIN_I2C_SDA 15

#define MOSU_PIN_LCD_CS 12
#define MOSU_PIN_LCD_PCLK 11
#define MOSU_PIN_LCD_D0 4
#define MOSU_PIN_LCD_D1 5
#define MOSU_PIN_LCD_D2 6
#define MOSU_PIN_LCD_D3 7
#define MOSU_PIN_LCD_RST 8

#define MOSU_PIN_TOUCH_RST 9
#define MOSU_PIN_TOUCH_INT 38

#define MOSU_PIN_SD_CLK 2
#define MOSU_PIN_SD_CMD 1
#define MOSU_PIN_SD_D0 3
/* The socket also carries the SPI chip select on DAT3/CS.  SD mode needs
 * that line high; leaving it floating can make a card enter SPI mode. */
#define MOSU_PIN_SD_CS 17

#define MOSU_PIN_I2S_MCLK 16
#define MOSU_PIN_I2S_BCLK 41
#define MOSU_PIN_I2S_LRCK 45
#define MOSU_PIN_I2S_DOUT 40
#define MOSU_PIN_PA_ENABLE 46

#define MOSU_PIN_IMU_INT 21
#define MOSU_PIN_RTC_INT 39

#define MOSU_LCD_H_RES 410
#define MOSU_LCD_V_RES 502
#define MOSU_LCD_X_GAP 0x16

#define MOSU_SD_MOUNT_POINT "/sdcard"
#define MOSU_CFG_PATH MOSU_SD_MOUNT_POINT "/mosu.cfg"

typedef struct {
    bool battery_present;
    bool vbus_present;
    bool charging;
    uint16_t millivolts;
    uint8_t percent;
} mosu_board_pmu_t;

typedef struct {
    float x;
    float y;
    float z;
} mosu_board_accel_t;

/*
 * One struct for the console's `display` line: the geometry and buffer sizes
 * are compile-time choices, brightness and power state come from the driver.
 * Strings are literals owned by the component, never freed by the caller.
 */
typedef struct {
    int32_t width;
    int32_t height;
    uint8_t brightness; /* configured percent, from the last set */
    bool on;
    uint32_t draw_buffer_pixels;
    bool double_buffer;
    bool buffer_dma;
    bool buffer_spiram;
    const char *panel;
    const char *touch;
} mosu_board_display_info_t;

esp_err_t mosu_board_init(void);

/* Display + touch + LVGL port.  Call once, after mosu_board_init(). */
esp_err_t mosu_board_display_start(void);
lv_display_t *mosu_board_display(void);
esp_err_t mosu_board_display_on(void);
/* Sleep out of the panel without switching the display on yet: the caller
 * draws the first frame while the panel is still dark, then calls display_on
 * so no stale or undefined GRAM content is ever visible. */
esp_err_t mosu_board_display_prepare_wake(void);
esp_err_t mosu_board_display_off(void);
esp_err_t mosu_board_brightness_set(uint8_t percent);
bool mosu_board_touch_active(void);
/* Read-only snapshot for diagnostics; never fails, never touches the panel. */
void mosu_board_display_info(mosu_board_display_info_t *out);

esp_err_t mosu_board_sdcard_mount(void);
/* Destructive: format the card as FAT32 and mount it again. */
esp_err_t mosu_board_sdcard_format(void);
void mosu_board_sdcard_unmount(void);
bool mosu_board_sdcard_ready(void);
/* Show the raw sdmmc/FAT driver logs around the next mount (bring-up). */
void mosu_board_sdcard_set_verbose(bool verbose);
/* Raw sector-0 dump: tells FAT32/MBR from exFAT/GPT/blank without a PC. */
esp_err_t mosu_board_sdcard_dump_boot(void);

esp_err_t mosu_board_pmu_read(mosu_board_pmu_t *out);

/* Cut the system rails through the AXP2101.  On battery it does not return;
 * with VBUS present the PMU may keep running to charge the cell. */
esp_err_t mosu_board_pmu_power_off(void);

esp_err_t mosu_board_rtc_init(void);
esp_err_t mosu_board_rtc_read(struct tm *utc);
esp_err_t mosu_board_rtc_write(const struct tm *utc);
bool mosu_board_rtc_valid(void);

esp_err_t mosu_board_imu_init(void);
esp_err_t mosu_board_imu_read(mosu_board_accel_t *out);

esp_err_t mosu_board_audio_init(void);
esp_err_t mosu_board_audio_play_wav(const char *path, uint8_t volume);
esp_err_t mosu_board_audio_beep(uint16_t hz, uint16_t ms);
/* Release the I2S clocks and the codec stream between announcements. */
esp_err_t mosu_board_audio_idle(void);

/* Allow the touch controller to wake the CPU from automatic light sleep. */
esp_err_t mosu_board_enable_touch_wake(bool enable);

/* ------------------------------------------------------------ diagnostics */
/*
 * Bring-up helpers for the console.  They only read state and print to
 * stdout: a missing part must produce a line of text, never a failed boot.
 */
esp_err_t mosu_board_i2c_scan(void);
void mosu_board_pmu_dump(void);
void mosu_board_rtc_dump(void);
void mosu_board_imu_dump(void);
void mosu_board_pm_dump(void);
void mosu_board_heap_dump(void);
void mosu_board_tasks_dump(void);

/* Three short beeps (440/880/1320 Hz) through the normal speaker path. */
esp_err_t mosu_board_audio_test(void);

/* Drop and mount the SD card again, waiting like the boot does. */
esp_err_t mosu_board_sdcard_remount(void);

#ifdef __cplusplus
}
#endif
