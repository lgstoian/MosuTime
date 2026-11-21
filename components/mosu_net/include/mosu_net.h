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
    MOSU_NET_FAILED,     /* the last attempt failed (Wi-Fi or SNTP) */
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

/* Replace the runtime network list with one network.  Used by the console
 * when there is no SD card to hold mosu.cfg; the caller persists the config
 * to NVS.  The password is never logged or printed. */
esp_err_t mosu_net_set_network(const char *ssid, const char *pass);
void mosu_net_clear_networks(void);

/* Bring-up helper: start the radio, scan, print SSID/channel/RSSI/authmode,
 * then turn the radio off again unless wifi_always_on is set. */
esp_err_t mosu_net_scan(void);

/* Bring-up helper: scan once and report whether any configured SSID is in
 * range, without associating. */
bool mosu_net_home_ap_visible(void);

/* Ask the network task to sync as soon as possible (used by the console). */
void mosu_net_request_sync(void);

/* Seconds until the next scheduled sync attempt. */
uint32_t mosu_net_seconds_to_sync(void);

/* Audit: total time the radio has been on, and a kill switch for A/B runs. */
uint32_t mosu_net_wifi_on_ms(void);
void mosu_net_set_enabled(bool enable);
bool mosu_net_enabled(void);

#ifdef __cplusplus
}
#endif
