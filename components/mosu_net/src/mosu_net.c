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
#include "mosu_policy.h"
#include "mosu_time.h"

static const char *TAG = "mosu_net";

#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1

static const mosu_config_t *s_cfg;
static mosu_config_t s_cfg_store;
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

/*
 * One timezone request.  retryable is only set for failures that are worth a
 * second attempt: a timeout, a connection error or an HTTP 5xx.  A 4xx means
 * the URL is wrong and retrying would just waste battery.
 */
static bool timezone_request(const char *url, char *body, size_t body_len,
                             bool *retryable)
{
    *retryable = false;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 8000,
        /* Plain HTTP is the default (cheaper, no certificate bundle), but a
         * user who configures https:// still gets a verified connection. */
        .crt_bundle_attach =
            strncmp(url, "https://", 8) == 0 ? esp_crt_bundle_attach : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return false;
    }

    body[0] = '\0';
    bool ok = false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "timezone request failed: %s", esp_err_to_name(err));
        *retryable = err == ESP_ERR_TIMEOUT || err == ESP_ERR_HTTP_CONNECT;
        goto done;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGW(TAG, "timezone response has no headers");
        *retryable = true;
        goto done;
    }

    int status = esp_http_client_get_status_code(client);
    if (status >= 500 && status <= 599) {
        ESP_LOGW(TAG, "timezone service returned HTTP %d", status);
        *retryable = true;
        goto done;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "timezone service returned HTTP %d", status);
        goto done;
    }

    int read = esp_http_client_read(client, body, (int)(body_len - 1));
    if (read <= 0) {
        ESP_LOGW(TAG, "timezone response was empty");
        *retryable = read < 0;
        goto done;
    }

    body[read] = '\0';
    ok = true;

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static bool fetch_timezone(mosu_policy_geo_t *geo)
{
    memset(geo, 0, sizeof(*geo));

    if (!s_cfg->clock.auto_timezone || s_cfg->clock.geo_url[0] == '\0') {
        return false;
    }

    const char *url = s_cfg->clock.geo_url;
    char body[256] = {0};
    bool got = false;

    /* One bounded retry.  The radio is already on, so a second request costs
     * far less than waiting hours for the next sync window. */
    for (int attempt = 0; attempt < 2; attempt++) {
        bool retryable = false;
        got = timezone_request(url, body, sizeof(body), &retryable);
        if (got) {
            if (attempt > 0) {
                ESP_LOGI(TAG, "timezone request succeeded on retry");
            }
            break;
        }
        if (!retryable || attempt == 1) {
            break;
        }
        ESP_LOGW(TAG, "transient timezone failure, retrying once");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!got) {
        ESP_LOGW(TAG, "timezone lookup failed, keeping %s",
                 mosu_time_timezone());
        return false;
    }

    mosu_policy_parse_geo(body, geo);
    if (geo->zone[0] == '\0' && geo->posix[0] == '\0') {
        ESP_LOGW(TAG, "timezone answer is not a zone: '%s'", body);
        return false;
    }

    if (geo->zone[0] && geo->posix[0]) {
        ESP_LOGI(TAG, "network timezone: %s (offset %s)", geo->zone,
                 geo->posix);
    } else if (geo->zone[0]) {
        ESP_LOGI(TAG, "network timezone: %s", geo->zone);
    } else {
        ESP_LOGI(TAG, "network timezone offset: %s", geo->posix);
    }
    return true;
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

static bool run_sync(void)
{
    if (s_cfg->wifi_count == 0) {
        s_state = MOSU_NET_IDLE;
        return true;
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
        return false;
    }

    s_state = MOSU_NET_CONNECTED;

    mosu_policy_geo_t geo;
    if (fetch_timezone(&geo)) {
        /* Prefer the name when the table knows it; otherwise the numeric
         * offset is still better than keeping a zone from another country. */
        bool applied = false;
        if (geo.zone[0]) {
            applied = mosu_time_apply_timezone(geo.zone);
            if (!applied && geo.posix[0]) {
                ESP_LOGI(TAG, "could not map '%s', using the numeric offset",
                         geo.zone);
            }
        }
        if (!applied && geo.posix[0]) {
            applied = mosu_time_apply_timezone(geo.posix);
        }
        if (!applied) {
            ESP_LOGW(TAG, "timezone lookup failed, keeping %s",
                     mosu_time_timezone());
        }
    }

    bool clock_ok = sync_clock();
    if (!clock_ok) {
        ESP_LOGW(TAG, "SNTP did not answer");
        s_state = MOSU_NET_FAILED;
    }

    radio_off_if_possible();
    if (!s_cfg->clock.wifi_always_on && clock_ok) {
        s_state = MOSU_NET_IDLE;
    }
    return clock_ok;
}

static void net_task(void *arg)
{
    (void)arg;
    s_next_sync_s = 0;
    uint8_t fail_count = 0;

    for (;;) {
        uint32_t now = uptime_s();
        if (s_sync_requested || (int32_t)(now - s_next_sync_s) >= 0) {
            s_sync_requested = false;
            bool ok = run_sync();
            if (ok) {
                fail_count = 0;
            } else if (fail_count < UINT8_MAX) {
                fail_count++;
            }

            uint32_t wait = mosu_policy_next_sync_s(
                fail_count, s_cfg->clock.sync_hours);
            s_next_sync_s = uptime_s() + wait;

            if (!ok) {
                ESP_LOGW(TAG, "sync failed (%u in a row), next try in %u s",
                         (unsigned)fail_count, (unsigned)wait);
            }
        }

        uint32_t wait_s = 60;
        if ((int32_t)(s_next_sync_s - now) > 0) {
            uint32_t until = s_next_sync_s - now;
            if (until < wait_s) {
                wait_s = until;
            }
        }

        /* Wake for a console sync request immediately; otherwise sleep in
         * one-minute steps so the radio task costs almost nothing. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000u + 100u));
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
    /* Work from a private copy: the console can change brightness and volume
     * in the live config while this task is running. */
    s_cfg_store = *cfg;
    s_cfg = &s_cfg_store;

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
