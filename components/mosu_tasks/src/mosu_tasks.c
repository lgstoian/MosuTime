/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

#include "mosu_tasks.h"

static const char *TAG = "mosu_tasks";

#define MOSU_TASKS_MAX 8

typedef struct {
    const char *name;
    TaskHandle_t handle;
    uint32_t stack;
} task_slot_t;

static task_slot_t s_tasks[MOSU_TASKS_MAX];

esp_err_t mosu_task_start(const mosu_task_cfg_t *cfg, TaskHandle_t *out)
{
    if (!cfg || !cfg->fn || !cfg->name || cfg->stack < 2048) {
        return ESP_ERR_INVALID_ARG;
    }

    TaskHandle_t handle = NULL;
    BaseType_t ok;
    if (cfg->psram_stack) {
        ok = xTaskCreatePinnedToCoreWithCaps(
            cfg->fn, cfg->name, cfg->stack, cfg->arg, cfg->priority, &handle,
            cfg->core, MALLOC_CAP_SPIRAM);
    } else {
        ok = xTaskCreatePinnedToCore(cfg->fn, cfg->name, cfg->stack, cfg->arg,
                                     cfg->priority, &handle, cfg->core);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "cannot create %s (%u bytes)", cfg->name,
                 (unsigned)cfg->stack);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < MOSU_TASKS_MAX; i++) {
        if (!s_tasks[i].handle) {
            s_tasks[i].name = cfg->name;
            s_tasks[i].handle = handle;
            s_tasks[i].stack = cfg->stack;
            break;
        }
    }
    if (out) {
        *out = handle;
    }
    return ESP_OK;
}

size_t mosu_task_stack_free(TaskHandle_t task)
{
    if (!task) {
        return 0;
    }
    return (size_t)uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
}

void mosu_tasks_report(void)
{
    for (int i = 0; i < MOSU_TASKS_MAX; i++) {
        if (!s_tasks[i].handle) {
            continue;
        }
        const size_t free_bytes = mosu_task_stack_free(s_tasks[i].handle);
        printf("task  : %-12s stack %5u, free %5u bytes\n", s_tasks[i].name,
               (unsigned)s_tasks[i].stack, (unsigned)free_bytes);
    }
    printf("mem   : internal free %u, largest %u, minimum ever %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    printf("mem   : psram free %u, largest %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
