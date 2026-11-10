/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * The SD card is optional: a dirty, missing or badly seated card must never
 * stop the watch from showing the time.  The blocking FAT mount therefore runs
 * in its own task and the boot waits only a few seconds for it.
 */
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
#define MOSU_SD_TASK_STACK 6144

static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_done;
static volatile bool s_ok;
static volatile bool s_running;

static esp_err_t mount_now(void)
{
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

    return esp_vfs_fat_sdmmc_mount(MOSU_SD_MOUNT_POINT, &host, &slot_cfg,
                                   &mount_cfg, &s_card);
}

static void mount_task(void *arg)
{
    (void)arg;

    esp_err_t err = mount_now();
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SD card mounted at %s", MOSU_SD_MOUNT_POINT);
    }

    s_ok = (err == ESP_OK);
    s_running = false;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t mosu_board_sdcard_mount(void)
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
    if (xTaskCreate(mount_task, "mosu_sd", MOSU_SD_TASK_STACK, NULL, 3, NULL) !=
        pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(MOSU_SD_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "SD card did not answer in %d ms, continuing without it",
                 MOSU_SD_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }

    return s_ok ? ESP_OK : ESP_FAIL;
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
