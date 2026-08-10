/*
 * wifi.c
 *
 * Always runs WIFI_MODE_APSTA: the fallback AP is not a timed failover, it
 * is simply always on. A panel-mounted diagnostic device that can go
 * silent because someone fat-fingered a WiFi password is worse than one
 * that spends a few extra mA holding a second radio interface up.
 *
 * STA reconnect uses a one-shot esp_timer rather than blocking inside the
 * event handler — the default event loop runs on a single shared task, and
 * sleeping there would delay every other WiFi/IP event in the system.
 */

#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";

#define AP_DEFAULT_PASSWORD     "cncmonitor"   /* WPA2 minimum is 8 chars */
#define AP_MAX_CONNECTIONS      4
#define AP_CHANNEL               1
#define STA_RECONNECT_DELAY_US  (5 * 1000 * 1000)

static esp_netif_t       *s_netif_ap;
static esp_timer_handle_t s_reconnect_timer;
static SemaphoreHandle_t  s_status_mutex;
static wifi_status_t      s_status;
static bool               s_sta_ssid_set;

static void ip4_to_str(const esp_ip4_addr_t *ip, char *out, size_t n)
{
    snprintf(out, n, IPSTR, IP2STR(ip));
}

/* strncpy's "may be truncated" warning (-Wstringop-truncation) is a false
 * positive for a deliberately truncating, always-null-terminated copy —
 * but GCC still errors on it here (-Werror), so this sidesteps strncpy
 * entirely rather than fighting the warning. */
static void str_copy_bounded(char *dst, size_t dst_size, const char *src)
{
    size_t len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_sta_ssid_set) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "STA reconnect attempt failed: %s", esp_err_to_name(err));
        }
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_ssid_set) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.sta_connected = false;
        s_status.sta_ip[0] = '\0';
        xSemaphoreGive(s_status_mutex);

        if (s_sta_ssid_set) {
            /* Fixed backoff, retried indefinitely. There is no need for an
             * exponential scheme or a retry ceiling here — the fallback AP
             * already guarantees the device is reachable, so a slow,
             * patient retry is strictly better than giving up. */
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, STA_RECONNECT_DELAY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.sta_connected = true;
        ip4_to_str(&evt->ip_info.ip, s_status.sta_ip, sizeof(s_status.sta_ip));
        xSemaphoreGive(s_status_mutex);
        ESP_LOGI(TAG, "STA connected, IP %s", s_status.sta_ip);
    }
}

static void apply_ap_config(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    wifi_config_t ap_cfg = {0};
    int len = snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid),
                        "CNC-Monitor-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap_cfg.ap.ssid_len = (uint8_t)len;
    str_copy_bounded((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password),
                     AP_DEFAULT_PASSWORD);
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = AP_MAX_CONNECTIONS;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    str_copy_bounded(s_status.ap_ssid, sizeof(s_status.ap_ssid), (const char *)ap_cfg.ap.ssid);
    xSemaphoreGive(s_status_mutex);

    ESP_LOGI(TAG, "fallback AP: SSID '%s'", ap_cfg.ap.ssid);
}

static void apply_sta_config(const app_config_t *cfg)
{
    const wifi_cfg_t *w = &cfg->system.wifi;

    wifi_config_t sta_cfg = {0};
    str_copy_bounded((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), w->ssid);
    str_copy_bounded((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), w->password);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    s_sta_ssid_set = (w->ssid[0] != '\0');
}

esp_err_t wifi_init(const app_config_t *cfg)
{
    s_status_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex) return ESP_ERR_NO_MEM;
    memset(&s_status, 0, sizeof(s_status));

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    apply_ap_config();
    apply_sta_config(cfg);
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ap_ip;
    if (esp_netif_get_ip_info(s_netif_ap, &ap_ip) == ESP_OK) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        ip4_to_str(&ap_ip.ip, s_status.ap_ip, sizeof(s_status.ap_ip));
        xSemaphoreGive(s_status_mutex);
    }

    return ESP_OK;
}

esp_err_t wifi_reconfigure(const app_config_t *cfg)
{
    esp_timer_stop(s_reconnect_timer);

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "disconnect before reconfigure: %s", esp_err_to_name(err));
    }

    apply_sta_config(cfg);

    if (s_sta_ssid_set) {
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "STA reconnect after reconfigure failed: %s",
                     esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "WiFi settings reloaded");
    return ESP_OK;
}

void wifi_get_status(wifi_status_t *out)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    memcpy(out, &s_status, sizeof(*out));
    xSemaphoreGive(s_status_mutex);
}
