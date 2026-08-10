/*
 * web.c
 *
 * Four routes:
 *   GET  /             the embedded settings + telemetry page
 *   GET  /api/config    current WiFi/Modbus config + connectivity status
 *   POST /api/config    apply a WiFi/Modbus config change
 *   GET  /api/telemetry per-spindle snapshot, polled by the page at 1 Hz
 *
 * No WebSocket: esp_http_server supports one, but broadcasting to every
 * connected client needs enumerating client fds and queuing async work per
 * client — real complexity for a diagnostic panel that only needs a 1 Hz
 * refresh. Polling /api/telemetry gets the same user-visible result (a
 * live readout) with far less code to get right without a bench to test
 * against yet.
 *
 * POST /api/config reuses the same commit path the rest of the system
 * already has: config_get_copy() -> edit -> config_store_commit()
 * (config_store.h) validates, seals and atomically swaps the live config,
 * persisting it with the known-good fallback that module already provides.
 * On success this task also calls wifi_reconfigure()/modbus_reconfigure()
 * so the change is live immediately, the same "no reboot needed" shape
 * monitor_config_changed() already established for the real-time task.
 */

#include "web.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "alarm.h"
#include "board.h"
#include "config_store.h"
#include "modbus.h"
#include "monitor.h"
#include "spindle_sm.h"
#include "wifi.h"

static const char *TAG = "web";

#define CONFIG_POST_MAX_LEN 1024

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ============================================================
 * Helpers
 * ============================================================ */

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

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    cJSON_free(out);
    cJSON_Delete(root);
    return err;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "reason", reason);
    return send_json(req, root);
}

/* ============================================================
 * GET /
 * ============================================================ */

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

/* ============================================================
 * GET /api/config
 * ============================================================ */

static esp_err_t handle_config_get(httpd_req_t *req)
{
    const app_config_t *cfg = config_get();
    const modbus_cfg_t *m = &cfg->system.modbus;

    wifi_status_t st;
    wifi_get_status(&st);

    cJSON *root = cJSON_CreateObject();

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    /* Password intentionally omitted: this device serves plain HTTP, and
     * echoing the secret back to every viewer of the page is unnecessary.
     * The form leaves it blank and only sends it on save. */
    cJSON_AddStringToObject(wifi, "ssid", cfg->system.wifi.ssid);
    cJSON_AddBoolToObject(wifi, "sta_connected", st.sta_connected);
    cJSON_AddStringToObject(wifi, "sta_ip", st.sta_ip);
    cJSON_AddStringToObject(wifi, "ap_ssid", st.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_ip", st.ap_ip);

    cJSON *mb = cJSON_AddObjectToObject(root, "modbus");
    cJSON_AddBoolToObject(mb, "rtu_enabled", m->rtu_enabled);
    cJSON_AddNumberToObject(mb, "baud", m->baud);
    cJSON_AddNumberToObject(mb, "parity", m->parity);
    cJSON_AddNumberToObject(mb, "stop_bits", m->stop_bits);
    cJSON_AddNumberToObject(mb, "data_bits", m->data_bits);
    cJSON_AddNumberToObject(mb, "slave_id", m->slave_id);
    cJSON_AddBoolToObject(mb, "tcp_enabled", m->tcp_enabled);
    cJSON_AddNumberToObject(mb, "tcp_port", m->tcp_port);

    return send_json(req, root);
}

/* ============================================================
 * POST /api/config
 * ============================================================ */

static esp_err_t handle_config_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= CONFIG_POST_MAX_LEN) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_OK;
    }

    char buf[CONFIG_POST_MAX_LEN];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
            return ESP_OK;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return send_json_error(req, "invalid JSON");
    }

    app_config_t working;
    config_get_copy(&working);

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(wifi, "ssid");
        if (cJSON_IsString(ssid)) {
            str_copy_bounded(working.system.wifi.ssid, CFG_WIFI_SSID_LEN, ssid->valuestring);
        }
        /* Password key is only sent by the page when the operator typed a
         * new one, so its absence here means "keep the current password" —
         * working already holds it via config_get_copy(). */
        cJSON *pass = cJSON_GetObjectItemCaseSensitive(wifi, "password");
        if (cJSON_IsString(pass)) {
            str_copy_bounded(working.system.wifi.password, CFG_WIFI_PASS_LEN, pass->valuestring);
        }
    }

    cJSON *mb = cJSON_GetObjectItemCaseSensitive(root, "modbus");
    if (cJSON_IsObject(mb)) {
        modbus_cfg_t *m = &working.system.modbus;
        cJSON *item;

        if ((item = cJSON_GetObjectItemCaseSensitive(mb, "rtu_enabled")) && cJSON_IsBool(item))
            m->rtu_enabled = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItemCaseSensitive(mb, "baud")) && cJSON_IsNumber(item))
            m->baud = (uint32_t)item->valuedouble;
        if ((item = cJSON_GetObjectItemCaseSensitive(mb, "parity")) && cJSON_IsNumber(item))
            m->parity = (cfg_parity_t)item->valueint;
        if ((item = cJSON_GetObjectItemCaseSensitive(mb, "stop_bits")) && cJSON_IsNumber(item))
            m->stop_bits = (uint8_t)item->valueint;
        if ((item = cJSON_GetObjectItemCaseSensitive(mb, "data_bits")) && cJSON_IsNumber(item))
            m->data_bits = (uint8_t)item->valueint;
        if ((item = cJSON_GetObjectItemCaseSensitive(mb, "slave_id")) && cJSON_IsNumber(item))
            m->slave_id = (uint8_t)item->valueint;
        if ((item = cJSON_GetObjectItemCaseSensitive(mb, "tcp_enabled")) && cJSON_IsBool(item))
            m->tcp_enabled = cJSON_IsTrue(item);
        if ((item = cJSON_GetObjectItemCaseSensitive(mb, "tcp_port")) && cJSON_IsNumber(item))
            m->tcp_port = (uint16_t)item->valuedouble;
    }

    cJSON_Delete(root);

    cfg_result_t reason = CFG_OK;
    esp_err_t err = config_store_commit(&working, &reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config POST rejected: %s", app_config_result_str(reason));
        return send_json_error(req, app_config_result_str(reason));
    }

    const app_config_t *live = config_get();
    wifi_reconfigure(live);
    modbus_reconfigure(live);
    monitor_config_changed();

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    return send_json(req, ok);
}

/* ============================================================
 * GET /api/telemetry
 * ============================================================ */

static esp_err_t handle_telemetry_get(httpd_req_t *req)
{
    monitor_snapshot_t snap;
    monitor_get_snapshot(&snap);
    const app_config_t *cfg = config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "system_healthy", snap.system_healthy);
    cJSON_AddBoolToObject(root, "adc_healthy", snap.adc_healthy);
    cJSON_AddBoolToObject(root, "diagnostic_fault", snap.diagnostic_fault);
    cJSON_AddNumberToObject(root, "loop_period_ms", snap.loop_period_ms);

    cJSON *spindles = cJSON_AddArrayToObject(root, "spindles");
    for (int i = 0; i < NUM_SPINDLES; i++) {
        const spindle_snapshot_t *sp = &snap.spindle[i];
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name", cfg->spindle[i].name);
        cJSON_AddBoolToObject(s, "enabled", cfg->spindle[i].enabled);
        cJSON_AddStringToObject(s, "state", spindle_state_str(sp->state));
        cJSON_AddBoolToObject(s, "monitoring_armed", sp->monitoring_armed);
        cJSON_AddNumberToObject(s, "current_a", sp->current_a);
        cJSON_AddNumberToObject(s, "current_avg_a", sp->current_avg_a);
        cJSON_AddNumberToObject(s, "current_peak_a", sp->current_peak_a);
        cJSON_AddNumberToObject(s, "pressure", sp->pressure);
        cJSON_AddNumberToObject(s, "rpm", sp->rpm);
        cJSON_AddStringToObject(s, "severity", severity_str(sp->severity));
        cJSON_AddBoolToObject(s, "any_alarm", sp->any_alarm);
        cJSON_AddBoolToObject(s, "any_warning", sp->any_warning);
        cJSON_AddItemToArray(spindles, s);
    }

    return send_json(req, root);
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t web_init(const app_config_t *cfg)
{
    (void)cfg;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = STACK_WEB;
    config.core_id = CORE_NETWORK;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",               .method = HTTP_GET,  .handler = handle_index },
        { .uri = "/api/config",     .method = HTTP_GET,  .handler = handle_config_get },
        { .uri = "/api/config",     .method = HTTP_POST, .handler = handle_config_post },
        { .uri = "/api/telemetry",  .method = HTTP_GET,  .handler = handle_telemetry_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "web UI listening on port %d", config.server_port);
    return ESP_OK;
}
