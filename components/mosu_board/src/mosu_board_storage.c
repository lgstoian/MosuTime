/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "mosu_board.h"

static const char *TAG = "mosu_sd";
static sdmmc_card_t *s_card;

esp_err_t mosu_board_sdcard_mount(void)
{
    if (s_card) {
        return ESP_OK;
    }

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
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

    esp_err_t err =
        esp_vfs_fat_sdmmc_mount(MOSU_SD_MOUNT_POINT, &host, &slot_cfg,
                                &mount_cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", MOSU_SD_MOUNT_POINT);
    return ESP_OK;
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
