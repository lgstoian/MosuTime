/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * CO5300 / SH8601 AMOLED over QSPI, FT3168 touch, LVGL 9 port.
 *
 * The panel has no backlight pin: brightness is a panel command (0x51) and the
 * display can be switched off entirely (0x28), which is what makes the
 * screen-off power story viable on an AMOLED.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mosu_board.h"
#include "mosu_board_priv.h"
#include "mosu_core/mosu_layout.h"

static const char *TAG = "mosu_disp";

#define LCD_OPCODE_WRITE_CMD 0x02u
#define LCD_SPI_NUM SPI2_HOST
/* Partial framebuffers of 50 lines: small enough for internal DMA memory,
 * large enough that LVGL rarely splits a full-screen redraw. */
#define DISP_BUF_LINES 50

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_display;
static bool s_display_on;
static bool s_panel_sleep;
static uint8_t s_brightness;

/* Vendor initialisation sequence for this panel revision. */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    /* Brightness is a config value: start dark and let app_main apply it
     * immediately after the display is ready, so there is no bright flash. */
    {0x51, (uint8_t[]){0x00}, 1, 0},
};

/*
 * The SH8601 panel resets on a low pulse.  Driving the pin here (instead of
 * through esp_lcd_panel_reset) keeps the timing explicit.
 */
static void panel_hardware_reset(void)
{
    gpio_set_level(MOSU_PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(MOSU_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_LOGI(TAG, "panel reset done");
}

static esp_err_t panel_create(void)
{
    const spi_bus_config_t bus_cfg =
        SH8601_PANEL_BUS_QSPI_CONFIG(MOSU_PIN_LCD_PCLK, MOSU_PIN_LCD_D0,
                                     MOSU_PIN_LCD_D1, MOSU_PIN_LCD_D2,
                                     MOSU_PIN_LCD_D3,
                                     MOSU_LCD_H_RES * 100 * 2);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_NUM, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg =
        SH8601_PANEL_IO_QSPI_CONFIG(MOSU_PIN_LCD_CS, NULL, NULL);
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_NUM, &io_cfg,
                                 &s_io),
        TAG, "panel io");

    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = MOSU_PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG,
                        "panel");
    ESP_LOGI(TAG, "panel object ready");
    panel_hardware_reset();
    ESP_LOGI(TAG, "panel reset done");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    ESP_LOGI(TAG, "panel init done");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, MOSU_LCD_X_GAP, 0), TAG,
                        "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");
    ESP_LOGI(TAG, "panel on");
    return ESP_OK;
}

static esp_err_t touch_create(void)
{
    const esp_lcd_touch_config_t cfg = {
        .x_max = MOSU_LCD_H_RES,
        .y_max = MOSU_LCD_V_RES,
        .rst_gpio_num = MOSU_PIN_TOUCH_RST,
        .int_gpio_num = MOSU_PIN_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_cfg.scl_speed_hz = 400000;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(mosu_board_i2c_bus(), &tp_cfg, &tp_io), TAG,
        "touch io");
    return esp_lcd_touch_new_i2c_ft5x06(tp_io, &cfg, &s_touch);
}

esp_err_t mosu_board_display_start(void)
{
    if (s_display) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(panel_create(), TAG, "panel");

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    /* A full-screen LVGL redraw (a new face, a scrolled list) runs the
     * renderer and the label cache on one stack; 8 KB ran short.  The
     * console's `tasks` command shows the real high-water mark on hardware,
     * so tune this from that number instead of guessing. */
    port_cfg.task_stack = 12288;
    port_cfg.task_affinity = 1;
    port_cfg.timer_period_ms = 30;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_io,
        .panel_handle = s_panel,
        /* Two 50-line buffers in internal DMA memory: the QSPI driver can
         * stream from them directly, and 82 KB is affordable next to Wi-Fi
         * and the TLS session used for the timezone lookup. */
        .buffer_size = MOSU_LCD_H_RES * DISP_BUF_LINES,
        .double_buffer = true,
        .hres = MOSU_LCD_H_RES,
        .vres = MOSU_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,
        },
    };

    /* esp_lvgl_port calls esp_lcd_panel_swap_xy() unconditionally, and the
     * SH8601 driver logs E even when swap_axes is false.  Silence that one
     * known false error for the duration of the call; keep the return value
     * check below as the real verdict. */
    esp_log_level_t sh8601_level = esp_log_level_get("sh8601");
    esp_log_level_set("sh8601", ESP_LOG_NONE);
    s_display = lvgl_port_add_disp(&disp_cfg);
    esp_log_level_set("sh8601", sh8601_level);
    if (!s_display) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(touch_create(), TAG, "touch");

    /* A slower monitor scan is the cheapest touch idle current there is: the
     * controller spends most of the night scanning for the first contact.
     * The vendor driver initialises the controller asynchronously, so give it
     * a moment before changing its registers. */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t tune = mosu_board_touch_tune(80, 1);
    if (tune != ESP_OK) {
        ESP_LOGW(TAG, "touch tuning failed: %s", esp_err_to_name(tune));
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_display,
        .handle = s_touch,
    };
    if (!lvgl_port_add_touch(&touch_cfg)) {
        ESP_LOGE(TAG, "lvgl_port_add_touch failed");
        return ESP_FAIL;
    }

    /* The touch interrupt is also a wake source. */
    /* The panel starts up with the face on, so the touch interrupt stays a
     * normal falling edge.  The level wake source is enabled in screen_off. */
    ESP_RETURN_ON_ERROR(mosu_board_enable_touch_wake(false), TAG, "wake");

    s_display_on = true;
    ESP_LOGI(TAG, "display ready (%dx%d)", MOSU_LCD_H_RES, MOSU_LCD_V_RES);
    return ESP_OK;
}

lv_display_t *mosu_board_display(void)
{
    return s_display;
}

esp_err_t mosu_board_brightness_set(uint8_t percent)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;

    uint8_t value = (uint8_t)(((uint32_t)percent * 255 + 50) / 100);
    uint32_t cmd = ((uint32_t)0x51 << 8) | ((uint32_t)LCD_OPCODE_WRITE_CMD << 24);
    return esp_lcd_panel_io_tx_param(s_io, (int)cmd, &value, 1);
}

/* Raw panel command, using the same opcode packing as the brightness call. */
static esp_err_t panel_cmd(uint8_t op)
{
    uint32_t cmd = ((uint32_t)op << 8) | ((uint32_t)LCD_OPCODE_WRITE_CMD << 24);
    return esp_lcd_panel_io_tx_param(s_io, (int)cmd, NULL, 0);
}

/* Paint the GRAM black in small DMA chunks: 410x502x2 bytes is 411 KB, too
 * much to allocate whole, and the panel is already off so this is invisible. */
static void panel_fill_black(void)
{
    static uint16_t line[MOSU_LCD_H_RES * 8];
    for (int y = 0; y < MOSU_LCD_V_RES; y += 8) {
        int h = MOSU_LCD_V_RES - y;
        if (h > 8) {
            h = 8;
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, MOSU_LCD_H_RES, y + h, line);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

esp_err_t mosu_board_display_on(void)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_display_on) {
        return ESP_OK;
    }
    if (s_panel_sleep) {
        /* ALDO2 is not a pull-up: it is the AMOLED daughterboard's supply
         * switch (the boost for ELVDD/ELVSS).  SLPIN alone leaves that boost
         * running and costs about 30 mA.  Gate it here; re-enable it before
         * the panel is woken. */
        ESP_RETURN_ON_ERROR(mosu_board_pmu_rail_set("aldo2", true), TAG,
                            "display rail");
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_RETURN_ON_ERROR(panel_cmd(0x11), TAG, "sleep out");
        vTaskDelay(pdMS_TO_TICKS(120));
        panel_fill_black();
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "on");
    s_display_on = true;
    return ESP_OK;
}

void mosu_board_display_set_panel_sleep(bool on)
{
    s_panel_sleep = on;
}

bool mosu_board_display_panel_sleep(void)
{
    return s_panel_sleep;
}

esp_err_t mosu_board_display_off(void)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_display_on) {
        return ESP_OK;
    }
    /* Dim first so the panel does not flash while switching off. */
    mosu_board_brightness_set(0);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, false), TAG, "off");
    if (s_panel_sleep) {
        /* The vendor sequence needs the display off before sleep-in.  The
         * GRAM is repainted black on the next wake, before display-on. */
        esp_err_t err = panel_cmd(0x10);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "panel sleep in: %s", esp_err_to_name(err));
        }
        /* Gate the panel supply (ALDO2) after SLPIN.  The CO5300 logic stays
         * powered from ALDO1, so its registers survive. */
        err = mosu_board_pmu_rail_set("aldo2", false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "panel rail off: %s", esp_err_to_name(err));
        }
    }
    s_display_on = false;
    return ESP_OK;
}

bool mosu_board_touch_active(void)
{
    return gpio_get_level(MOSU_PIN_TOUCH_INT) == 0;
}

void mosu_board_display_info(mosu_board_display_info_t *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->width = MOSU_LCD_H_RES;
    out->height = MOSU_LCD_V_RES;
    out->brightness = s_brightness;
    out->on = s_display_on;
    out->panel_sleep = s_panel_sleep;
    /* These come from the fixed configuration above, not from the driver:
     * lvgl_port does not expose its buffer allocation back to us. */
    out->draw_buffer_pixels = MOSU_LCD_H_RES * DISP_BUF_LINES;
    out->double_buffer = true;
    out->buffer_dma = true;
    out->buffer_spiram = false;
    out->panel = "SH8601/CO5300";
    out->touch = "FT3168";
}

esp_err_t mosu_board_enable_touch_wake(bool enable)
{
    esp_err_t err = ESP_OK;

    if (enable) {
        /* Light sleep only accepts a level trigger.  While the face is off we
         * do not poll the touch controller, so the level is what wakes us. */
        err = gpio_wakeup_enable(MOSU_PIN_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
        if (err == ESP_OK) {
            err = esp_sleep_enable_gpio_wakeup();
        }
    } else {
        err = gpio_wakeup_disable(MOSU_PIN_TOUCH_INT);
        /* gpio_wakeup_enable() changes the pin to level-low; the LVGL touch
         * ISR expects the original falling edge.  Put it back. */
        if (gpio_set_intr_type(MOSU_PIN_TOUCH_INT, GPIO_INTR_NEGEDGE) !=
            ESP_OK) {
            ESP_LOGW(TAG, "could not restore the touch edge interrupt");
        }
    }
    return err;
}
