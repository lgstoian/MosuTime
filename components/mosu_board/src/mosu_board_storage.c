/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The SD card is optional: a dirty, missing or badly seated card must never
 * stop the watch from showing the time.  The blocking FAT mount therefore runs
 * in its own task and the boot waits only a few seconds for it.
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "mosu_board.h"

static const char *TAG = "mosu_sd";

#define MOSU_SD_WAIT_MS 3000
/* Formatting a 64 GB card rewrites the FAT area, so it gets its own window. */
#define MOSU_SD_FORMAT_WAIT_MS 120000
#define MOSU_SD_TASK_STACK 6144

static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_done;
static volatile bool s_ok;
static volatile bool s_running;
static volatile bool s_verbose;

static esp_err_t mount_now(bool format)
{
    /* The TF socket is also wired for SPI, with CS on GPIO17.  In SD mode
     * that pin is DAT3/CS and must be high, otherwise a floating line can
     * make the card start in SPI mode and the SDMMC init fails.  The
     * SDMMC host uses 1-bit width, so it does not drive GPIO17 itself. */
    const gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << MOSU_PIN_SD_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cs_cfg) == ESP_OK) {
        gpio_set_level(MOSU_PIN_SD_CS, 1);
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = format,
        .max_files = 6,
        .allocation_unit_size = format ? 32 * 1024 : 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 20000; /* slow and reliable; we only read tiny files */

    const sdmmc_slot_config_t slot_cfg = {
        .clk = MOSU_PIN_SD_CLK,
        .cmd = MOSU_PIN_SD_CMD,
        .d0 = MOSU_PIN_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

    /* No card is a normal operating state for this watch: the driver logs
     * two E lines for the expected OCR timeout, which makes a clean boot
     * log impossible.  Keep the code from mosu_sd (it logs the result) and
     * suppress only the raw driver tags around the attempt. */
    esp_log_level_t sdmmc_level = ESP_LOG_NONE;
    esp_log_level_t fat_level = ESP_LOG_NONE;
    if (!s_verbose) {
        sdmmc_level = esp_log_level_get("sdmmc_common");
        fat_level = esp_log_level_get("vfs_fat_sdmmc");
        esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
        esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    }

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOSU_SD_MOUNT_POINT, &host,
                                            &slot_cfg, &mount_cfg, &s_card);

    if (!s_verbose) {
        esp_log_level_set("sdmmc_common", sdmmc_level);
        esp_log_level_set("vfs_fat_sdmmc", fat_level);
    }
    return err;
}

void mosu_board_sdcard_set_verbose(bool verbose)
{
    s_verbose = verbose;
}

esp_err_t mosu_board_sdcard_dump_boot(void)
{
    if (s_card || s_running) {
        printf("sd dump: unmount the card first\n");
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 20000;

    const sdmmc_slot_config_t slot_cfg = {
        .clk = MOSU_PIN_SD_CLK,
        .cmd = MOSU_PIN_SD_CMD,
        .d0 = MOSU_PIN_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

    esp_err_t err = host.init();
    if (err != ESP_OK) {
        printf("sd dump: host init failed (%s)\n", esp_err_to_name(err));
        return err;
    }
    err = sdmmc_host_init_slot(host.slot, &slot_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("sd dump: slot init failed (%s)\n", esp_err_to_name(err));
        return err;
    }

    sdmmc_card_t card = {0};
    err = sdmmc_card_init(&host, &card);
    if (err != ESP_OK) {
        printf("sd dump: card init failed (%s)\n", esp_err_to_name(err));
        return err;
    }

    sdmmc_card_print_info(stdout, &card);
    uint8_t sector[512];
    err = sdmmc_read_sectors(&card, sector, 0, 1);
    if (err != ESP_OK) {
        printf("sd dump: sector 0 read failed (%s)\n", esp_err_to_name(err));
        return err;
    }

    printf("sd dump: sector 0 (%u bytes)\n", (unsigned)card.csd.sector_size);
    for (int i = 0; i < 512; i += 16) {
        printf("%04X:", i);
        for (int j = 0; j < 16; j++) {
            printf(" %02X", sector[i + j]);
        }
        printf("  ");
        for (int j = 0; j < 16; j++) {
            unsigned char c = sector[i + j];
            putchar(c >= 32 && c < 127 ? c : '.');
        }
        putchar('\n');
    }

    if (sector[510] == 0x55 && sector[511] == 0xAA) {
        uint32_t part_lba = (uint32_t)sector[0x1C6] |
                            ((uint32_t)sector[0x1C7] << 8) |
                            ((uint32_t)sector[0x1C8] << 16) |
                            ((uint32_t)sector[0x1C9] << 24);
        uint32_t part_sectors = (uint32_t)sector[0x1CA] |
                                ((uint32_t)sector[0x1CB] << 8) |
                                ((uint32_t)sector[0x1CC] << 16) |
                                ((uint32_t)sector[0x1CD] << 24);
        printf("sd dump: MBR type 0x%02X, start LBA %u, %u sectors\n",
               sector[0x1C2], (unsigned)part_lba, (unsigned)part_sectors);

        if (part_lba > 0 && part_lba < card.csd.capacity) {
            uint8_t boot[512];
            err = sdmmc_read_sectors(&card, boot, part_lba, 1);
            if (err == ESP_OK) {
                const uint32_t total = (uint32_t)boot[0x20] |
                                       ((uint32_t)boot[0x21] << 8) |
                                       ((uint32_t)boot[0x22] << 16) |
                                       ((uint32_t)boot[0x23] << 24);
                const uint32_t fat_sectors = (uint32_t)boot[0x24] |
                                             ((uint32_t)boot[0x25] << 8) |
                                             ((uint32_t)boot[0x26] << 16) |
                                             ((uint32_t)boot[0x27] << 24);
                const uint32_t root_cluster = (uint32_t)boot[0x2C] |
                                              ((uint32_t)boot[0x2D] << 8) |
                                              ((uint32_t)boot[0x2E] << 16) |
                                              ((uint32_t)boot[0x2F] << 24);
                printf("sd dump: partition boot %02X %02X %02X, bps %u, "
                       "spc %u, rsvd %u, fats %u, fat32 '%.8s', sig %02X%02X\n",
                       boot[0], boot[1], boot[2],
                       (unsigned)(boot[11] | (boot[12] << 8)),
                       (unsigned)boot[13],
                       (unsigned)(boot[14] | (boot[15] << 8)),
                       (unsigned)boot[16], (const char *)(boot + 0x52),
                       boot[510], boot[511]);
                printf("sd dump: total %u sectors, FAT %u sectors, root %u\n",
                       (unsigned)total, (unsigned)fat_sectors,
                       (unsigned)root_cluster);
                printf("sd dump: fsver 0x%04X, root entries %u, FAT16 size %u\n",
                       (unsigned)(boot[0x2A] | (boot[0x2B] << 8)),
                       (unsigned)(boot[0x11] | (boot[0x12] << 8)),
                       (unsigned)(boot[0x16] | (boot[0x17] << 8)));
            } else {
                printf("sd dump: partition boot read failed (%s)\n",
                       esp_err_to_name(err));
            }
        }
    }
    putchar('\n');
    return ESP_OK;
}

static void mount_task(void *arg)
{
    const bool format = arg != NULL;

    esp_err_t err = mount_now(format);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SD card %s at %s",
                 format ? "formatted and mounted" : "mounted",
                 MOSU_SD_MOUNT_POINT);
    }

    s_ok = (err == ESP_OK);
    s_running = false;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static esp_err_t start_mount(bool format, uint32_t wait_ms)
{
    if (s_card) {
        return ESP_OK;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_MOSU_SKIP_SD
    ESP_LOGW(TAG, "SD mount disabled at build time");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
        if (!s_done) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_running = true;
    if (xTaskCreate(mount_task, "mosu_sd", MOSU_SD_TASK_STACK,
                    format ? (void *)1 : NULL, 3, NULL) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "SD card did not answer in %u ms, continuing without it",
                 (unsigned)wait_ms);
        return ESP_ERR_TIMEOUT;
    }

    return s_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t mosu_board_sdcard_mount(void)
{
    return start_mount(false, MOSU_SD_WAIT_MS);
}

esp_err_t mosu_board_sdcard_format(void)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    mosu_board_sdcard_unmount();
    if (s_done) {
        while (xSemaphoreTake(s_done, 0) == pdTRUE) {
        }
    }
    return start_mount(true, MOSU_SD_FORMAT_WAIT_MS);
}

void mosu_board_sdcard_unmount(void)
{
    if (!s_card) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(MOSU_SD_MOUNT_POINT, s_card);
    s_card = NULL;
}

bool mosu_board_sdcard_ready(void)
{
    return s_card != NULL;
}

esp_err_t mosu_board_sdcard_remount(void)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    mosu_board_sdcard_unmount();

    /* A previous mount may have finished after its caller gave up waiting.
     * Drop that stale token, or the next mount would report the old result
     * without touching the card. */
    if (s_done) {
        while (xSemaphoreTake(s_done, 0) == pdTRUE) {
        }
    }

    return mosu_board_sdcard_mount();
}
