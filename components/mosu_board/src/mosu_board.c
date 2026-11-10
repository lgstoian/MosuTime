/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mosu_board.h"
#include "mosu_board_priv.h"

static const char *TAG = "mosu_board";

/* Enough for the IDF system tasks, the watch tasks and the console. */
#define MOSU_TASKS_MAX 24

esp_err_t mosu_board_init(void)
{
    esp_err_t err = mosu_board_i2c_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mosu_board_pmu_init();
    if (err != ESP_OK) {
        /* A dead PMU still allows a demo, but no battery information. */
        ESP_LOGE(TAG, "PMU init failed: %s", esp_err_to_name(err));
    }

    err = mosu_board_rtc_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC init failed: %s", esp_err_to_name(err));
    }

    err = mosu_board_imu_init();
    if (err != ESP_OK) {
        /* Wrist raise is a comfort feature; touch wake still works. */
        ESP_LOGW(TAG, "IMU init failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

/* ------------------------------------------------------------ diagnostics */

/*
 * Power management, heap and task state belong to the chip, not to a
 * peripheral, so they live here rather than next to the I2C drivers.
 */

void mosu_board_pm_dump(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t cfg = {0};
    if (esp_pm_get_configuration(&cfg) == ESP_OK) {
        printf("pm  : max %d MHz, min %d MHz, light sleep %d\n",
               cfg.max_freq_mhz, cfg.min_freq_mhz,
               (int)cfg.light_sleep_enable);
    } else {
        printf("pm  : configuration not available\n");
    }

    esp_err_t err = esp_pm_dump_locks(stdout);
    if (err != ESP_OK) {
        printf("pm  : lock dump failed (%s)\n", esp_err_to_name(err));
    }
#else
    printf("pm  : CONFIG_PM_ENABLE is off, no frequency scaling\n");
#endif
}

static void print_heap(const char *label, uint32_t caps)
{
    size_t free_bytes = heap_caps_get_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    printf("heap: %-9s free %u, largest %u\n", label, (unsigned)free_bytes,
           (unsigned)largest);
}

void mosu_board_heap_dump(void)
{
    print_heap("internal", MALLOC_CAP_INTERNAL);
    /* PSRAM is optional; without it the PSRAM caps report zero, which is
     * still the truthful answer. */
    print_heap("psram", MALLOC_CAP_SPIRAM);
}

void mosu_board_tasks_dump(void)
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    /* One snapshot is enough for a bring-up check; the array is static so
     * that a nearly full heap cannot turn a diagnostic into a failure. */
    static TaskStatus_t status[MOSU_TASKS_MAX];
    UBaseType_t wanted = uxTaskGetNumberOfTasks();
    if (wanted > MOSU_TASKS_MAX) {
        wanted = MOSU_TASKS_MAX;
    }

    UBaseType_t count = uxTaskGetSystemState(status, wanted, NULL);
    /* The high-water mark is in bytes: ESP-IDF defines StackType_t as a byte
     * so that stack sizes can be given in bytes everywhere. */
    printf("tasks: %u listed\n", (unsigned)count);
    printf("      %-16s %3s %10s\n", "name", "pri", "stack free");
    for (UBaseType_t i = 0; i < count; i++) {
        printf("      %-16s %3u %10u\n", status[i].pcTaskName,
               (unsigned)status[i].uxCurrentPriority,
               (unsigned)status[i].usStackHighWaterMark);
    }
#else
    printf("tasks: uxTaskGetSystemState needs "
           "CONFIG_FREERTOS_USE_TRACE_FACILITY\n");
#endif
}
