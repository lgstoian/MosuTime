/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Task and memory policy in one place.
 *
 * FreeRTOS stacks are fixed, so "dynamic" here means measured: every task is
 * created through this module, registered, and its high-water mark can be
 * printed.  Large buffers may live in PSRAM, but task stacks default to
 * internal RAM: a PSRAM stack is unsafe while the flash cache is disabled
 * (NVS writes) and slower on every call.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    TaskFunction_t fn;
    void *arg;
    uint32_t stack;      /* bytes, not words */
    UBaseType_t priority;
    BaseType_t core;     /* tskNO_AFFINITY, 0 or 1 */
    bool psram_stack;    /* only for tasks that never write flash */
} mosu_task_cfg_t;

/* Create a task with the given policy and remember it for the report. */
esp_err_t mosu_task_start(const mosu_task_cfg_t *cfg, TaskHandle_t *out);

/* Bytes never used on the stack (high-water mark). */
size_t mosu_task_stack_free(TaskHandle_t task);

/* Print name, stack size and headroom for every registered task. */
void mosu_tasks_report(void);

#ifdef __cplusplus
}
#endif
