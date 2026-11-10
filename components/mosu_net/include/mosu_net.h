/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mosu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOSU_NET_IDLE = 0,   /* waiting for the next window, or no networks */
    MOSU_NET_CONNECTING, /* trying the configured access points */
    MOSU_NET_CONNECTED,  /* associated, time freshly synced */
    MOSU_NET_FAILED,     /* every configured access point failed */
} mosu_net_state_t;

typedef struct {
    mosu_net_state_t state;
    bool have_sync;
    uint32_t sync_age_s;
    int8_t rssi;
    char ip[16];
} mosu_net_status_t;

esp_err_t mosu_net_start(const mosu_config_t *cfg);

void mosu_net_status(mosu_net_status_t *out);

/* Ask the network task to sync as soon as possible (used by the console). */
void mosu_net_request_sync(void);

/* Seconds until the next scheduled sync attempt. */
uint32_t mosu_net_seconds_to_sync(void);

#ifdef __cplusplus
}
#endif
