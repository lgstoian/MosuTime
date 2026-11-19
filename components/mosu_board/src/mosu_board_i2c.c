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
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mosu_board.h"
#include "mosu_board_priv.h"
#include "mosu_core/mosu_core.h"

static const char *TAG = "mosu_board";

#define AXP2101_ADDR 0x34
#define PCF85063_ADDR 0x51
/* The Waveshare wiki says 0x6A, but the populated QMI8658C has SA0 high and
 * answers at 0x6B on the real board (confirmed with the `i2c` scan). */
#define QMI8658_ADDR 0x6B

/* PCF85063A register map.  This is not the PCF8563 layout: 0x02/0x03 are
 * the offset and RAM registers, and the time starts at 0x04.  Using the
 * PCF8563 addresses shifted every read and wrote the time into the wrong
 * registers (symptom: an hour register counting like seconds). */
#define PCF85063_SEC 0x04
#define PCF85063_MIN 0x05
#define PCF85063_HOUR 0x06
#define PCF85063_DAY 0x07
#define PCF85063_WEEKDAY 0x08
#define PCF85063_MONTH 0x09
#define PCF85063_YEAR 0x0A

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pmu;
static i2c_master_dev_handle_t s_rtc;
static i2c_master_dev_handle_t s_imu;

static uint8_t s_rtc_year;
static uint8_t s_rtc_month;
static uint8_t s_rtc_day;
static uint8_t s_rtc_hour;
static bool s_rtc_ok;
static bool s_rtc_os; /* oscillator-stop flag: the time is not trustworthy */

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
    /* One combined transaction: the register address and the read must be
     * separated by a repeated START.  The PCF85063 resets its address
     * pointer when it sees a STOP, so transmit()+receive() made every RTC
     * read start at register 0x00 and return shifted bytes. */
    return i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100);
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
    /* TS pin control: external fixed input, no NTC.  Clearing the ADC
     * channel alone is not enough; the charger still looks at the TS pin
     * configuration unless 0x50 selects the fixed-input mode (bit 4). */
    ESP_RETURN_ON_ERROR(pmu_update(0x50, 0x1F, 0x10), TAG, "pmu ts");
    /* Battery detection. */
    ESP_RETURN_ON_ERROR(pmu_update(0x68, 0x01, 0x01), TAG, "pmu batdet");
    /* Charge target 4.2 V (CV field value 3; 2 would be 4.1 V). */
    ESP_RETURN_ON_ERROR(pmu_update(0x64, 0x07, 0x03), TAG, "pmu cv");
    /* Conservative charge profile until the cell is known: 200 mA constant
     * current, 25 mA termination, 50 mA precharge.  Adjust for the actual
     * capacity before the first full charge. */
    ESP_RETURN_ON_ERROR(pmu_update(0x62, 0x1F, 0x08), TAG, "pmu ichg");
    ESP_RETURN_ON_ERROR(pmu_update(0x63, 0x0F, 0x01), TAG, "pmu iterm");
    ESP_RETURN_ON_ERROR(pmu_update(0x61, 0x0F, 0x02), TAG, "pmu ipre");

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
    ESP_RETURN_ON_ERROR(reg_read(s_rtc, PCF85063_SEC, raw, sizeof(raw)), TAG,
                        "rtc");

    s_rtc_os = (raw[0] & 0x80) != 0;
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

    ESP_RETURN_ON_ERROR(reg_write(s_rtc, PCF85063_SEC, raw, sizeof(raw)), TAG,
                        "rtc w");
    s_rtc_ok = true;
    s_rtc_os = false;
    return ESP_OK;
}

bool mosu_board_rtc_valid(void)
{
    if (!s_rtc_ok || s_rtc_os || s_rtc_year < 24 || s_rtc_year > 99) {
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

    /* CTRL1: address auto-increment, little endian.
     *
     * Bit 6 is ADDR_AI and bit 5 is BE.  0x60 would enable address
     * auto-increment *and* big-endian output, while mosu_board_imu_read()
     * decodes the six bytes as little-endian int16 pairs.  That byte-swaps
     * every axis: at rest |a| reads as ~0.004 g instead of 1 g and the
     * wrist-raise detector never fires.  0x40 is the little-endian form. */
    ESP_RETURN_ON_ERROR(reg_write_u8(s_imu, 0x02, 0x40), TAG, "imu ctrl1");

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

/* ------------------------------------------------------------ diagnostics */

static const char *i2c_name(uint8_t address)
{
    switch (address) {
    case AXP2101_ADDR:
        return "AXP2101 PMU";
    case PCF85063_ADDR:
        return "PCF85063 RTC";
    case QMI8658_ADDR:
        return "QMI8658 IMU";
    case 0x38:
        return "FT3168 touch";
    case 0x18:
        return "ES8311 codec";
    case 0x40:
        return "ES7210 mic ADC";
    default:
        return NULL;
    }
}

esp_err_t mosu_board_i2c_scan(void)
{
    if (!s_bus) {
        printf("i2c : bus not started\n");
        return ESP_ERR_INVALID_STATE;
    }

    printf("i2c : scan SDA %d / SCL %d, 0x08..0x77\n", MOSU_PIN_I2C_SDA,
           MOSU_PIN_I2C_SCL);

    int found = 0;
    for (uint8_t address = 0x08; address <= 0x77; address++) {
        if (i2c_master_probe(s_bus, address, 50) != ESP_OK) {
            continue;
        }
        const char *name = i2c_name(address);
        printf("      0x%02X  %s\n", address, name ? name : "unknown device");
        found++;
    }

    printf("i2c : %d device(s) answered\n", found);
    /* An empty bus is a finding, not an error: the table above already says
     * what is missing. */
    return ESP_OK;
}

void mosu_board_pmu_dump(void)
{
    if (!s_pmu) {
        printf("pmu : not initialised\n");
        return;
    }

    /* The first status bytes cover the supply and charge state; they are
     * printed raw so the console can be compared with the datasheet. */
    for (uint8_t reg = 0x00; reg <= 0x02; reg++) {
        uint8_t value = 0;
        esp_err_t err = reg_read(s_pmu, reg, &value, 1);
        if (err != ESP_OK) {
            printf("pmu : register 0x%02X read failed (%s)\n", reg,
                   esp_err_to_name(err));
            return;
        }
        printf("pmu : reg 0x%02X = 0x%02X\n", reg, value);
    }

    /* Charging-related registers: TS pin mode, precharge/constant current,
     * termination current and CV target.  Printing them makes the battery
     * bring-up check possible without a vendor tool. */
    static const uint8_t charge_regs[] = {0x50, 0x61, 0x62, 0x63, 0x64};
    for (size_t i = 0; i < sizeof(charge_regs) / sizeof(charge_regs[0]); i++) {
        uint8_t value = 0;
        esp_err_t err = reg_read(s_pmu, charge_regs[i], &value, 1);
        if (err == ESP_OK) {
            printf("pmu : reg 0x%02X = 0x%02X\n", charge_regs[i], value);
        }
    }

    mosu_board_pmu_t pmu;
    esp_err_t err = mosu_board_pmu_read(&pmu);
    if (err != ESP_OK) {
        printf("pmu : decode failed (%s)\n", esp_err_to_name(err));
        return;
    }

    printf("pmu : battery %s, vbus %s, charging %s\n",
           pmu.battery_present ? "present" : "absent",
           pmu.vbus_present ? "present" : "absent",
           pmu.charging ? "yes" : "no");
    printf("pmu : %u mV, %u%%\n", (unsigned)pmu.millivolts,
           (unsigned)pmu.percent);
}

void mosu_board_rtc_dump(void)
{
    if (!s_rtc) {
        printf("rtc : not initialised\n");
        return;
    }

    uint8_t ctrl[3] = {0};
    esp_err_t err = reg_read(s_rtc, 0x00, ctrl, sizeof(ctrl));
    if (err != ESP_OK) {
        printf("rtc : control read failed (%s)\n", esp_err_to_name(err));
        return;
    }

    uint8_t raw[7];
    err = reg_read(s_rtc, PCF85063_SEC, raw, sizeof(raw));
    if (err != ESP_OK) {
        printf("rtc : read failed (%s)\n", esp_err_to_name(err));
        return;
    }

    printf("rtc : ctrl 0x00=%02X 0x01=%02X offset=%02X\n", ctrl[0], ctrl[1],
           ctrl[2]);
    printf("rtc : 0x04..0x0A = %02X %02X %02X %02X %02X %02X %02X\n", raw[0],
           raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]);

    struct tm utc;
    if (mosu_board_rtc_read(&utc) != ESP_OK) {
        printf("rtc : decode failed\n");
        return;
    }

    printf("rtc : UTC %04d-%02d-%02d %02d:%02d:%02d, valid %d\n",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
           utc.tm_min, utc.tm_sec, (int)mosu_board_rtc_valid());
}

void mosu_board_imu_dump(void)
{
    if (!s_imu) {
        printf("imu : not initialised\n");
        return;
    }

    uint8_t who = 0;
    esp_err_t err = reg_read(s_imu, 0x00, &who, 1);
    if (err != ESP_OK) {
        printf("imu : WHO_AM_I read failed (%s)\n", esp_err_to_name(err));
        return;
    }
    printf("imu : WHO_AM_I 0x%02X (%s)\n", who,
           who == 0x05 ? "QMI8658" : "unexpected value");

    mosu_board_accel_t accel;
    err = mosu_board_imu_read(&accel);
    if (err != ESP_OK) {
        printf("imu : accel read failed (%s)\n", esp_err_to_name(err));
        return;
    }

    float magnitude =
        sqrtf(accel.x * accel.x + accel.y * accel.y + accel.z * accel.z);
    printf("imu : accel %.3f %.3f %.3f g, |a| %.3f g\n", accel.x, accel.y,
           accel.z, magnitude);
    /* At rest the magnitude is 1 g; a different value means the scale or
     * the mounting orientation is not what the datasheet promises. */
}

/* --------------------------------------------------------------- boot IO */

/*
 * A slave that lost power in the middle of a transfer can keep SDA low by
 * holding the last bit it received.  The I2C peripheral refuses to start on a
 * bus that does not look idle, so every probe would fail until the next reset.
 *
 * Nine clock pulses let such a slave finish the byte it is stuck in and see a
 * NACK; the closing STOP (SDA rising while SCL is high) then returns the bus to
 * the idle state.  This runs only before the driver owns the pins.
 */
static void bus_recover(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask =
            (1ULL << MOSU_PIN_I2C_SDA) | (1ULL << MOSU_PIN_I2C_SCL),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "I2C recovery pins could not be configured");
        return;
    }

    /* Both lines idle high; give a slow pull-up time to charge the bus. */
    gpio_set_level(MOSU_PIN_I2C_SDA, 1);
    gpio_set_level(MOSU_PIN_I2C_SCL, 1);
    esp_rom_delay_us(20);

    if (gpio_get_level(MOSU_PIN_I2C_SDA) == 0) {
        ESP_LOGW(TAG, "SDA held low, clocking the bus free");
        for (int i = 0; i < 9 && gpio_get_level(MOSU_PIN_I2C_SDA) == 0; i++) {
            gpio_set_level(MOSU_PIN_I2C_SCL, 0);
            esp_rom_delay_us(5);
            gpio_set_level(MOSU_PIN_I2C_SCL, 1);
            esp_rom_delay_us(5);
        }

        /* STOP: SDA low then high while SCL stays high. */
        gpio_set_level(MOSU_PIN_I2C_SDA, 0);
        esp_rom_delay_us(5);
        gpio_set_level(MOSU_PIN_I2C_SCL, 1);
        esp_rom_delay_us(5);
        gpio_set_level(MOSU_PIN_I2C_SDA, 1);
        esp_rom_delay_us(5);

        if (gpio_get_level(MOSU_PIN_I2C_SDA) == 0) {
            /* The short on SDA is not a stuck slave: check the hardware. */
            ESP_LOGW(TAG, "SDA still low, I2C will probably fail");
        }
    }

    /* Hand the pins back untouched so i2c_new_master_bus() configures them
     * exactly the way its own driver expects. */
    gpio_reset_pin(MOSU_PIN_I2C_SDA);
    gpio_reset_pin(MOSU_PIN_I2C_SCL);
}

esp_err_t mosu_board_i2c_start(void)
{
    if (s_bus) {
        return ESP_OK;
    }

    bus_recover();

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
