/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Minimal drivers for the three I2C parts the watch actually needs:
 *   AXP2101  PMU   0x34  - rails, battery voltage, charge state
 *   PCF85063 RTC   0x51  - seconds..years, UTC
 *   QMI8658  IMU   0x6A  - accelerometer for the wrist raise
 *
 * They are deliberately tiny: every register used here is documented in the
 * source, so the code can be audited without a vendor library.
 */
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mosu_board.h"
#include "mosu_board_priv.h"
#include "mosu_core/mosu_core.h"

static const char *TAG = "mosu_board";

#define AXP2101_ADDR 0x34
#define PCF85063_ADDR 0x51
#define QMI8658_ADDR 0x6A

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pmu;
static i2c_master_dev_handle_t s_rtc;
static i2c_master_dev_handle_t s_imu;

static uint8_t s_rtc_year;
static uint8_t s_rtc_month;
static uint8_t s_rtc_day;
static uint8_t s_rtc_hour;
static bool s_rtc_ok;

i2c_master_bus_handle_t mosu_board_i2c_bus(void)
{
    return s_bus;
}

static esp_err_t bus_add(uint8_t address, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, out);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf,
                          size_t len)
{
    esp_err_t err = i2c_master_transmit(dev, &reg, 1, 100);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_receive(dev, buf, len, 100);
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg,
                           const uint8_t *buf, size_t len)
{
    uint8_t data[8];
    if (len > sizeof(data) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    data[0] = reg;
    memcpy(data + 1, buf, len);
    return i2c_master_transmit(dev, data, len + 1, 100);
}

static esp_err_t reg_write_u8(i2c_master_dev_handle_t dev, uint8_t reg,
                              uint8_t value)
{
    return reg_write(dev, reg, &value, 1);
}

/* ------------------------------------------------------------------ PMU */

static esp_err_t pmu_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    ESP_RETURN_ON_ERROR(reg_read(s_pmu, reg, &current, 1), TAG, "pmu read");
    current = (uint8_t)((current & ~mask) | (value & mask));
    return reg_write_u8(s_pmu, reg, current);
}

esp_err_t mosu_board_pmu_init(void)
{
    esp_err_t err = bus_add(AXP2101_ADDR, &s_pmu);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t status = 0;
    err = reg_read(s_pmu, 0x00, &status, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not responding");
        return err;
    }

    /* Enable VBAT / VBUS / VSYS ADCs, disable the TS pin measurement.
     * The board has no battery thermistor; leaving TS enabled breaks
     * charging. */
    ESP_RETURN_ON_ERROR(pmu_update(0x30, 0x0F, 0x0D), TAG, "pmu adc");
    /* Battery detection. */
    ESP_RETURN_ON_ERROR(pmu_update(0x68, 0x01, 0x01), TAG, "pmu batdet");

    ESP_LOGI(TAG, "AXP2101 ready (status 0x%02X)", status);
    return ESP_OK;
}

esp_err_t mosu_board_pmu_read(mosu_board_pmu_t *out)
{
    if (!out || !s_pmu) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));

    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t raw[2];
    ESP_RETURN_ON_ERROR(reg_read(s_pmu, 0x00, &status1, 1), TAG, "pmu st1");
    ESP_RETURN_ON_ERROR(reg_read(s_pmu, 0x01, &status2, 1), TAG, "pmu st2");

    out->battery_present = (status1 & 0x08) != 0;
    out->vbus_present = (status2 & 0x08) == 0 && (status1 & 0x20) != 0;

    uint8_t charge = (uint8_t)(status2 & 0x07);
    out->charging = charge >= 1 && charge <= 3; /* precharge, CC, CV */

    if (out->battery_present) {
        /* 13 bit ADC value, 1 mV/LSB: 5 bits in the high byte, 8 in the low. */
        ESP_RETURN_ON_ERROR(reg_read(s_pmu, 0x34, raw, 2), TAG, "pmu vbat");
        out->millivolts = (uint16_t)(((raw[0] & 0x1F) << 8) | raw[1]);
        out->percent = mosu_battery_percent(out->millivolts);
    } else {
        out->millivolts = 0;
        out->percent = 0;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ RTC */

static uint8_t bcd_to_bin(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t bin_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

esp_err_t mosu_board_rtc_init(void)
{
    esp_err_t err = bus_add(PCF85063_ADDR, &s_rtc);
    if (err != ESP_OK) {
        return err;
    }

    /* Control_1 = 0: oscillator running, 24 hour mode, no test mode. */
    err = reg_write_u8(s_rtc, 0x00, 0x00);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not responding: %s", esp_err_to_name(err));
        return err;
    }

    struct tm now;
    if (mosu_board_rtc_read(&now) == ESP_OK) {
        s_rtc_ok = true;
    }
    return ESP_OK;
}

esp_err_t mosu_board_rtc_read(struct tm *utc)
{
    if (!utc || !s_rtc) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7];
    ESP_RETURN_ON_ERROR(reg_read(s_rtc, 0x02, raw, sizeof(raw)), TAG, "rtc");

    uint8_t sec = bcd_to_bin(raw[0] & 0x7F);
    uint8_t min = bcd_to_bin(raw[1] & 0x7F);
    uint8_t hour_reg = raw[2];
    uint8_t hour;
    if (hour_reg & 0x40) {
        /* 12 hour mode */
        hour = bcd_to_bin(hour_reg & 0x1F);
        if (hour_reg & 0x20) {
            hour = (uint8_t)((hour % 12) + 12);
        } else if (hour == 12) {
            hour = 0;
        }
    } else {
        hour = bcd_to_bin(hour_reg & 0x3F);
    }
    uint8_t day = bcd_to_bin(raw[3] & 0x3F);
    uint8_t month = bcd_to_bin(raw[5] & 0x1F);
    uint8_t year = bcd_to_bin(raw[6]);

    s_rtc_year = year;
    s_rtc_month = month;
    s_rtc_day = day;
    s_rtc_hour = hour;

    memset(utc, 0, sizeof(*utc));
    utc->tm_sec = sec;
    utc->tm_min = min;
    utc->tm_hour = hour;
    utc->tm_mday = day;
    utc->tm_mon = month - 1;
    utc->tm_year = 2000 + year - 1900;
    utc->tm_isdst = 0;
    return ESP_OK;
}

esp_err_t mosu_board_rtc_write(const struct tm *utc)
{
    if (!utc || !s_rtc) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7];
    raw[0] = bin_to_bcd((uint8_t)utc->tm_sec) & 0x7F;
    raw[1] = bin_to_bcd((uint8_t)utc->tm_min) & 0x7F;
    raw[2] = bin_to_bcd((uint8_t)utc->tm_hour) & 0x3F; /* 24 h */
    raw[3] = bin_to_bcd((uint8_t)utc->tm_mday) & 0x3F;
    raw[4] = (uint8_t)(utc->tm_wday & 0x07); /* free running counter */
    raw[5] = bin_to_bcd((uint8_t)(utc->tm_mon + 1)) & 0x1F;
    raw[6] = bin_to_bcd((uint8_t)(utc->tm_year % 100));

    ESP_RETURN_ON_ERROR(reg_write(s_rtc, 0x02, raw, sizeof(raw)), TAG, "rtc w");
    s_rtc_ok = true;
    return ESP_OK;
}

bool mosu_board_rtc_valid(void)
{
    if (!s_rtc_ok || s_rtc_year < 24 || s_rtc_year > 99) {
        return false;
    }
    if (s_rtc_month < 1 || s_rtc_month > 12) {
        return false;
    }
    if (s_rtc_day < 1 || s_rtc_day > 31) {
        return false;
    }
    if (s_rtc_hour > 23) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ IMU */

static uint16_t s_imu_lsb_div = 8192; /* +-4 g */

esp_err_t mosu_board_imu_init(void)
{
    esp_err_t err = bus_add(QMI8658_ADDR, &s_imu);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t who = 0;
    err = reg_read(s_imu, 0x00, &who, 1);
    if (err != ESP_OK || who != 0x05) {
        ESP_LOGW(TAG, "QMI8658 not found (who=0x%02X)", who);
        return ESP_ERR_NOT_FOUND;
    }

    /* CTRL1: address auto-increment, little endian. */
    ESP_RETURN_ON_ERROR(reg_write_u8(s_imu, 0x02, 0x60), TAG, "imu ctrl1");

    /* CTRL2: +-4 g, 21 Hz low power ODR.  The IMU only runs while the
     * screen is off; the low ODR keeps its average current negligible. */
    const uint8_t range_4g = 0x01;
    const uint8_t odr_lowpower_21hz = 0x0D;
    ESP_RETURN_ON_ERROR(
        reg_write_u8(s_imu, 0x03, (uint8_t)((range_4g << 4) | odr_lowpower_21hz)),
        TAG, "imu ctrl2");

    /* CTRL7: accelerometer on. */
    ESP_RETURN_ON_ERROR(reg_write_u8(s_imu, 0x08, 0x01), TAG, "imu ctrl7");

    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "QMI8658 ready");
    return ESP_OK;
}

esp_err_t mosu_board_imu_read(mosu_board_accel_t *out)
{
    if (!out || !s_imu) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[6];
    ESP_RETURN_ON_ERROR(reg_read(s_imu, 0x35, raw, sizeof(raw)), TAG, "imu");

    int16_t x = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t y = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t z = (int16_t)((raw[5] << 8) | raw[4]);

    out->x = (float)x / (float)s_imu_lsb_div;
    out->y = (float)y / (float)s_imu_lsb_div;
    out->z = (float)z / (float)s_imu_lsb_div;
    return ESP_OK;
}

/* --------------------------------------------------------------- boot IO */

esp_err_t mosu_board_i2c_start(void)
{
    if (s_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = MOSU_PIN_I2C_SDA,
        .scl_io_num = MOSU_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}
