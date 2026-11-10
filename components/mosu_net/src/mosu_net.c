/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * Wi-Fi + SNTP + IP geolocation.
 *
 * Default behaviour is "sync_only": wake the radio, associate, discover the
 * timezone, take one SNTP sample, write the RTC, switch the radio off again.
 * That is the difference between a watch that needs charging every day and one
 * that lasts for weeks.
 */
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "mosu_net.h"
#include "mosu_time.h"

static const char *TAG = "mosu_net";

#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1

static const mosu_config_t *s_cfg;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static volatile bool s_sync_requested;
static volatile bool s_wifi_started;
static volatile mosu_net_state_t s_state = MOSU_NET_IDLE;
static volatile int8_t s_rssi;
static char s_ip[16];
static uint32_t s_next_sync_s;

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_CONNECTED) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_events, BIT_CONNECTED);
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_rssi = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
        }
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

static bool wifi_ensure_started(void)
{
    if (s_wifi_started) {
        return true;
    }
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi start: %s", esp_err_to_name(err));
        return false;
    }
    s_wifi_started = true;
    return true;
}

static bool connect_to(const mosu_wifi_cfg_t *net, uint32_t timeout_ms)
{
    if (!wifi_ensure_started()) {
        return false;
    }

    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", net->ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", net->pass);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        return false;
    }

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    s_state = MOSU_NET_CONNECTING;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_CONNECTED) != 0;
}

static bool fetch_timezone(char *out, size_t out_len)
{
    if (!s_cfg->clock.auto_timezone || s_cfg->clock.geo_url[0] == '\0') {
        return false;
    }

    const char *url = s_cfg->clock.geo_url;
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach =
            strncmp(url, "https://", 8) == 0 ? esp_crt_bundle_attach : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return false;
    }

    char body[256] = {0};
    bool ok = false;

    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int read = esp_http_client_read(client, body, sizeof(body) - 1);
        if (read > 0) {
            body[read] = '\0';

            const char *marker = strstr(body, "\"timezone\"");
            if (marker) {
                const char *colon = strchr(marker, ':');
                const char *first = colon ? strchr(colon, '"') : NULL;
                const char *last = first ? strchr(first + 1, '"') : NULL;
                if (first && last && last > first + 1) {
                    size_t len = (size_t)(last - first - 1);
                    if (len < out_len) {
                        memcpy(out, first + 1, len);
                        out[len] = '\0';
                        ok = true;
                    }
                }
            } else {
                /* ipapi.co/timezone answers with plain text. */
                char *start = body;
                while (*start && (*start == ' ' || *start == '\n' ||
                                  *start == '\r' || *start == '\t')) {
                    start++;
                }
                char *finish = start + strlen(start);
                while (finish > start && (finish[-1] == '\n' ||
                                          finish[-1] == '\r' ||
                                          finish[-1] == ' ' ||
                                          finish[-1] == '\t')) {
                    *--finish = '\0';
                }
                if (*start && (size_t)(finish - start) < out_len) {
                    memcpy(out, start, (size_t)(finish - start) + 1);
                    ok = true;
                }
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ok) {
        ESP_LOGI(TAG, "network timezone: %s", out);
    } else {
        ESP_LOGW(TAG, "timezone lookup failed, keeping %s",
                 mosu_time_timezone());
    }
    return ok;
}

static bool sync_clock(void)
{
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST(s_cfg->clock.ntp1, s_cfg->clock.ntp2));

    if (esp_netif_sntp_init(&sntp_cfg) != ESP_OK) {
        return false;
    }

    bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK;
    esp_netif_sntp_deinit();

    if (ok) {
        mosu_time_set_epoch(time(NULL));
    }
    return ok;
}

static void radio_off_if_possible(void)
{
    if (s_cfg->clock.wifi_always_on) {
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_wifi_started = false;
}

static void run_sync(void)
{
    if (s_cfg->wifi_count == 0) {
        s_state = MOSU_NET_IDLE;
        return;
    }

    bool connected = false;
    for (uint8_t i = 0; i < s_cfg->wifi_count && !connected; i++) {
        connected = connect_to(&s_cfg->wifi[i], 15000);
        if (!connected) {
            ESP_LOGW(TAG, "cannot join '%s'", s_cfg->wifi[i].ssid);
        }
    }

    if (!connected) {
        s_state = MOSU_NET_FAILED;
        radio_off_if_possible();
        ESP_LOGW(TAG, "all configured networks failed");
        return;
    }

    s_state = MOSU_NET_CONNECTED;

    char tz[64];
    if (fetch_timezone(tz, sizeof(tz))) {
        mosu_time_apply_timezone(tz);
    }

    if (!sync_clock()) {
        ESP_LOGW(TAG, "SNTP did not answer");
    }

    radio_off_if_possible();
    if (!s_cfg->clock.wifi_always_on) {
        s_state = MOSU_NET_IDLE;
    }
}

static void net_task(void *arg)
{
    (void)arg;
    s_next_sync_s = 0;

    for (;;) {
        uint32_t now = uptime_s();
        if (s_sync_requested || (int32_t)(now - s_next_sync_s) >= 0) {
            s_sync_requested = false;
            run_sync();
            s_next_sync_s = uptime_s() + (uint32_t)s_cfg->clock.sync_hours * 3600u;
        }

        uint32_t wait_ms = 5000;
        if (s_cfg->clock.wifi_always_on) {
            wait_ms = 5000;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t mosu_net_start(const mosu_config_t *cfg)
{
    if (s_task) {
        return ESP_OK;
    }
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = cfg;

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, NULL, NULL),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, NULL, NULL),
        TAG, "ip handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), TAG, "ps");

    if (xTaskCreate(net_task, "mosu_net", 6144, NULL, 4, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mosu_net_status(mosu_net_status_t *out)
{
    if (!out) {
        return;
    }
    out->state = s_state;
    out->have_sync = mosu_time_sync_age_s() != UINT32_MAX;
    out->sync_age_s = mosu_time_sync_age_s();
    out->rssi = s_rssi;
    snprintf(out->ip, sizeof(out->ip), "%s", s_ip);
}

void mosu_net_request_sync(void)
{
    s_sync_requested = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

uint32_t mosu_net_seconds_to_sync(void)
{
    uint32_t now = uptime_s();
    if ((int32_t)(s_next_sync_s - now) <= 0) {
        return 0;
    }
    return s_next_sync_s - now;
}
