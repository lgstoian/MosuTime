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
#include <stdlib.h>
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
/* Association is not enough: the geolocation request needs an IP address,
 * so wait for DHCP before returning from connect_to(). */
#define BIT_GOT_IP BIT2

static mosu_config_t *s_cfg;
static mosu_config_t s_cfg_store;
static EventGroupHandle_t s_events;
static TaskHandle_t s_task;
static volatile bool s_sync_requested;
static volatile bool s_wifi_started;
static volatile mosu_net_state_t s_state = MOSU_NET_IDLE;
static volatile int8_t s_rssi;
static char s_ip[16];
static uint32_t s_next_sync_s;
static uint32_t s_next_presence_s;
static uint8_t s_last_reason;
static bool s_last_connected;

/* Presence scans are driven by the learned per-hour score in mosu_policy:
 * 15 minutes when the watch is normally home at this hour, up to 4 hours
 * when it never is. */

/* The console may replace the network list while the net task is reading it.
 * The strings are small, so a short spinlock around the copy is enough. */
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

static void copy_network(uint8_t index, char *ssid, size_t ssid_len, char *pass,
                         size_t pass_len)
{
    taskENTER_CRITICAL(&s_cfg_mux);
    snprintf(ssid, ssid_len, "%s", s_cfg->wifi[index].ssid);
    snprintf(pass, pass_len, "%s", s_cfg->wifi[index].pass);
    taskEXIT_CRITICAL(&s_cfg_mux);
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

static uint32_t sync_interval_s(void)
{
    if (s_cfg->clock.sync_per_day > 0) {
        return 86400u / s_cfg->clock.sync_per_day;
    }
    return (uint32_t)s_cfg->clock.sync_hours * 3600u;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_CONNECTED) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *ev =
                (const wifi_event_sta_disconnected_t *)data;
            s_last_reason = ev->reason;
            ESP_LOGW(TAG, "disconnected: reason %u", (unsigned)ev->reason);
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
        s_last_reason = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED | BIT_GOT_IP);
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

static bool connect_to(uint8_t index, uint32_t timeout_ms)
{
    s_last_reason = 0;
    if (!wifi_ensure_started()) {
        return false;
    }

    char ssid[MOSU_CFG_SSID_MAX];
    char pass[MOSU_CFG_PASS_MAX];
    copy_network(index, ssid, sizeof(ssid), pass, sizeof(pass));

    wifi_config_t wc = {0};
    /* wifi_sta_config_t has no separate length fields in ESP-IDF, so keep
     * one byte for the terminator.  The console setter rejects anything
     * longer; this clamp only protects a config file from a malformed
     * value. */
    size_t ssid_len = strlen(ssid);
    if (ssid_len >= sizeof(wc.sta.ssid)) {
        ssid_len = sizeof(wc.sta.ssid) - 1;
    }
    memcpy(wc.sta.ssid, ssid, ssid_len);

    size_t pass_len = strlen(pass);
    if (pass_len >= sizeof(wc.sta.password)) {
        pass_len = sizeof(wc.sta.password) - 1;
    }
    memcpy(wc.sta.password, pass, pass_len);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        return false;
    }

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_GOT_IP | BIT_FAILED);
    s_state = MOSU_NET_CONNECTING;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        return false;
    }

    /* Wait for DHCP, not just association: http_client's getaddrinfo()
     * fails with no address (the symptom was "couldn't get hostname for
     * ip-api.com" a second after joining). */
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_GOT_IP) != 0;
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
    s_last_connected = false;
    if (s_cfg->wifi_count == 0) {
        s_state = MOSU_NET_IDLE;
        return true;
    }

    uint8_t count = 0;
    taskENTER_CRITICAL(&s_cfg_mux);
    count = s_cfg->wifi_count;
    taskEXIT_CRITICAL(&s_cfg_mux);

    bool connected = false;
    for (uint8_t i = 0; i < count && !connected; i++) {
        connected = connect_to(i, 15000);
        if (!connected) {
            char ssid[MOSU_CFG_SSID_MAX];
            char pass[MOSU_CFG_PASS_MAX];
            copy_network(i, ssid, sizeof(ssid), pass, sizeof(pass));
            ESP_LOGW(TAG, "cannot join '%s'", ssid);
        }
    }

    if (!connected) {
        s_state = MOSU_NET_FAILED;
        radio_off_if_possible();
        ESP_LOGW(TAG, "all configured networks failed");
        s_last_connected = false;
        return false;
    }

    s_last_connected = true;
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

static bool home_ap_visible(void)
{
    char ssids[MOSU_CFG_WIFI_MAX][MOSU_CFG_SSID_MAX];
    uint8_t count = 0;

    taskENTER_CRITICAL(&s_cfg_mux);
    count = s_cfg->wifi_count;
    for (uint8_t i = 0; i < count && i < MOSU_CFG_WIFI_MAX; i++) {
        snprintf(ssids[i], sizeof(ssids[i]), "%s", s_cfg->wifi[i].ssid);
    }
    taskEXIT_CRITICAL(&s_cfg_mux);

    if (count == 0 || !wifi_ensure_started()) {
        return false;
    }

    wifi_scan_config_t scan_cfg = {.show_hidden = true};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        radio_off_if_possible();
        return false;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found > 20) {
        found = 20;
    }
    wifi_ap_record_t *aps =
        calloc(found ? found : 1, sizeof(wifi_ap_record_t));
    if (!aps) {
        radio_off_if_possible();
        return false;
    }

    bool visible = false;
    uint16_t got = found;
    if (esp_wifi_scan_get_ap_records(&got, aps) == ESP_OK) {
        for (uint16_t i = 0; i < got && !visible; i++) {
            for (uint8_t j = 0; j < count; j++) {
                if (strncmp((const char *)aps[i].ssid, ssids[j], 32) == 0) {
                    visible = true;
                    break;
                }
            }
        }
    }
    free(aps);
    radio_off_if_possible();
    return visible;
}

static void net_task(void *arg)
{
    (void)arg;
    s_next_sync_s = 0;
    uint8_t fail_count = 0;
    mosu_wifi_learn_t learn;
    mosu_wifi_learn_init(&learn);

    for (;;) {
        uint32_t now = uptime_s();
        struct tm local;
        mosu_time_now(&local);
        uint8_t hour = (uint8_t)(local.tm_hour % 24);

        if (s_sync_requested || (int32_t)(now - s_next_sync_s) >= 0) {
            s_sync_requested = false;
            bool ok = run_sync();
            mosu_wifi_learn_note(&learn, hour, ok);
            if (ok) {
                fail_count = 0;
            } else if (fail_count < UINT8_MAX) {
                fail_count++;
            }

            uint32_t wait =
                mosu_policy_next_sync_s(fail_count, sync_interval_s());
            s_next_sync_s = uptime_s() + wait;

            if (ok) {
                s_next_presence_s = 0;
            } else {
                /* If no access point answered at all, look for a configured
                 * SSID again.  The interval follows what this local hour
                 * usually looks like: 15 minutes when the watch is normally
                 * home at this hour, up to 4 hours when it never is.  When
                 * the owner comes home and the radio sees the AP, sync
                 * immediately instead of waiting for the full backoff.  An
                 * auth failure means the AP is visible: keep the normal,
                 * slower backoff so a wrong password cannot spin the radio. */
                bool no_ap = s_last_reason == 0 ||
                             s_last_reason == WIFI_REASON_NO_AP_FOUND;
                if (!s_last_connected && no_ap && s_cfg->wifi_count > 0 &&
                    !s_cfg->clock.wifi_always_on) {
                    uint32_t scan = mosu_wifi_learn_scan_s(&learn, hour);
                    s_next_presence_s = uptime_s() + scan;
                    ESP_LOGI(TAG, "presence scan in %u min (hour %u score %u)",
                             (unsigned)(scan / 60u), (unsigned)hour,
                             (unsigned)mosu_wifi_learn_score(&learn, hour));
                } else {
                    s_next_presence_s = 0;
                }
                ESP_LOGW(TAG, "sync failed (%u in a row), next try in %u s",
                         (unsigned)fail_count, (unsigned)wait);
            }
        }

        if (s_next_presence_s != 0 &&
            (int32_t)(uptime_s() - s_next_presence_s) >= 0) {
            s_next_presence_s = 0;
            bool visible = home_ap_visible();
            mosu_wifi_learn_note(&learn, hour, visible);
            if (visible) {
                ESP_LOGI(TAG, "configured network is visible again, syncing");
                s_sync_requested = true;
                s_next_sync_s = uptime_s();
                continue;
            }
            uint32_t scan = mosu_wifi_learn_scan_s(&learn, hour);
            s_next_presence_s = uptime_s() + scan;
            ESP_LOGI(TAG, "still away at hour %u (score %u), next scan in %u "
                     "min", (unsigned)hour,
                     (unsigned)mosu_wifi_learn_score(&learn, hour),
                     (unsigned)(scan / 60u));
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

esp_err_t mosu_net_scan(void)
{
    if (!s_cfg) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == MOSU_NET_CONNECTING || s_state == MOSU_NET_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_ensure_started()) {
        return ESP_FAIL;
    }

    wifi_scan_config_t scan_cfg = {.show_hidden = true};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start: %s", esp_err_to_name(err));
        radio_off_if_possible();
        return err;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 20) {
        count = 20;
    }
    wifi_ap_record_t *aps =
        calloc(count ? count : 1, sizeof(wifi_ap_record_t));
    if (!aps) {
        radio_off_if_possible();
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = count;
    err = esp_wifi_scan_get_ap_records(&got, aps);
    if (err == ESP_OK) {
        printf("wifi: %u AP(s)\n", (unsigned)got);
        for (uint16_t i = 0; i < got; i++) {
            printf("      %-32s ch %2u rssi %4d auth %d\n",
                   (const char *)aps[i].ssid, (unsigned)aps[i].primary,
                   (int)aps[i].rssi, (int)aps[i].authmode);
        }
    }
    free(aps);
    radio_off_if_possible();
    return err;
}

esp_err_t mosu_net_set_network(const char *ssid, const char *pass)
{
    if (!s_cfg || !ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= sizeof(((wifi_sta_config_t *)0)->ssid) ||
        (pass && strlen(pass) >= sizeof(((wifi_sta_config_t *)0)->password))) {
        return ESP_ERR_INVALID_SIZE;
    }

    taskENTER_CRITICAL(&s_cfg_mux);
    memset(&s_cfg->wifi[0], 0, sizeof(s_cfg->wifi[0]));
    snprintf(s_cfg->wifi[0].ssid, sizeof(s_cfg->wifi[0].ssid), "%s", ssid);
    snprintf(s_cfg->wifi[0].pass, sizeof(s_cfg->wifi[0].pass), "%s",
             pass ? pass : "");
    for (uint8_t i = 1; i < MOSU_CFG_WIFI_MAX; i++) {
        memset(&s_cfg->wifi[i], 0, sizeof(s_cfg->wifi[i]));
    }
    s_cfg->wifi_count = 1;
    taskEXIT_CRITICAL(&s_cfg_mux);

    return ESP_OK;
}

void mosu_net_clear_networks(void)
{
    if (!s_cfg) {
        return;
    }
    taskENTER_CRITICAL(&s_cfg_mux);
    memset(s_cfg->wifi, 0, sizeof(s_cfg->wifi));
    s_cfg->wifi_count = 0;
    taskEXIT_CRITICAL(&s_cfg_mux);
}

bool mosu_net_home_ap_visible(void)
{
    if (!s_cfg || s_state == MOSU_NET_CONNECTING ||
        s_state == MOSU_NET_CONNECTED) {
        return false;
    }
    return home_ap_visible();
}
