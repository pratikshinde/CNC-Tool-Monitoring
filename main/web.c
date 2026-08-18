/*
 * web.c — HTTP API behind the embedded operator interface.
 *
 *   GET  /                    the embedded single-page UI
 *   GET  /api/telemetry       live per-spindle snapshot (polled at 1 Hz)
 *   GET  /api/history         trend ring buffer for the graph
 *   GET  /api/system          version, uptime, heap, partition, reset cause
 *   GET  /api/config          WiFi + Modbus settings and link status
 *   POST /api/config          apply WiFi/Modbus settings
 *   GET  /api/spindle         full per-spindle config (bands + calibration)
 *   POST /api/thresholds      apply one spindle's threshold bands
 *   POST /api/calibration     apply one spindle's sensor setup / raw trims
 *   GET  /api/calib           calibration wizard state + live raw readings
 *   POST /api/calib           capture / apply / reset a calibration point
 *   POST /api/autozero        re-tare a CT channel
 *   POST /api/acknowledge     clear latched alarms
 *   POST /api/ota             stream a firmware image into the spare slot
 *   POST /api/reboot          restart
 *   POST /api/factory-reset   restore defaults
 *
 * No WebSocket: esp_http_server supports one, but broadcasting to every
 * connected client needs enumerating client fds and queuing async work per
 * client — real complexity for a diagnostic panel that only needs a 1 Hz
 * refresh. Polling gets the same user-visible result with far less code to
 * get wrong.
 *
 * Every write goes through the same commit path the rest of the system
 * uses: config_get_copy() -> edit -> config_store_commit()
 * (config_store.h) validates, seals and atomically swaps the live config,
 * persisting it with known-good fallback. Handlers never write config
 * fields directly.
 *
 * Config is split across several endpoints rather than one large blob
 * because a full configuration is ~150 numbers; per-section payloads keep
 * each request small enough to parse without heap pressure.
 */

#include "web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "alarm.h"
#include "board.h"
#include "calib.h"
#include "config_store.h"
#include "job_store.h"
#include "job_template.h"
#include "modbus.h"
#include "monitor.h"
#include "ota.h"
#include "scaling.h"
#include "spindle_sm.h"
#include "trend.h"
#include "wifi.h"

static const char *TAG = "web";

/* Largest configuration payload we will accept. A single spindle's full
 * threshold set (3 quantities x 4 bands x 6 fields) is around 2.5 KB of
 * JSON; 8 KB leaves comfortable headroom. Heap, not stack — the HTTP task
 * stack is nowhere near this size. */
#define BODY_MAX_LEN     8192

/* Chunk size for streaming a firmware upload. Bigger is fewer round trips,
 * but this buffer is live for the whole upload. */
#define OTA_CHUNK_LEN    2048

/* Chunk size for streaming the (already gzipped) index page out of the web
 * partition. Same reasoning as OTA_CHUNK_LEN: bigger is fewer round trips
 * against a small stack-resident buffer. */
#define WEB_FS_CHUNK_LEN 1024

#define WEB_FS_MOUNT     "/web"
#define WEB_FS_PARTITION "web"

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

static const char *sensor_status_str(sensor_status_t s)
{
    switch (s) {
    case SENSOR_OK:         return "ok";
    case SENSOR_OPEN:       return "open circuit";
    case SENSOR_OVERRANGE:  return "over range";
    case SENSOR_UNDERRANGE: return "under range";
    default:                return "?";
    }
}

static const char *link_state_str(smu_link_state_t s)
{
    switch (s) {
    case SMU_LINK_DOWN:     return "down";
    case SMU_LINK_DEGRADED: return "degraded";
    case SMU_LINK_LIVE:     return "live";
    default:                return "?";
    }
}

static const char *reset_cause_str(uint8_t c)
{
    switch (c) {
    case SMU_RESET_POWER_ON:  return "power-on";
    case SMU_RESET_WATCHDOG:  return "watchdog";
    case SMU_RESET_BROWNOUT:  return "brown-out";
    case SMU_RESET_SOFTWARE:  return "software";
    case SMU_RESET_EXTERNAL:  return "external";
    default:                   return "?";
    }
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

static esp_err_t send_status_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    return send_json(req, root);
}

static esp_err_t send_json_error(httpd_req_t *req, const char *reason)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "reason", reason);
    return send_json(req, root);
}

/* Read a whole request body into a freshly allocated, null-terminated
 * buffer. Caller frees. Returns NULL and answers the request on failure. */
static char *read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= BODY_MAX_LEN) {
        send_json_error(req, "request body missing or too large");
        return NULL;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        send_json_error(req, "out of memory");
        return NULL;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            send_json_error(req, "read error");
            return NULL;
        }
        received += r;
    }
    buf[received] = '\0';
    return buf;
}

/* Fetch an unsigned query parameter, e.g. ?spindle=1. */
static bool query_uint(httpd_req_t *req, const char *key, uint32_t *out)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[16];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return false;
    }
    *out = (uint32_t)strtoul(value, NULL, 10);
    return true;
}

/* Spindle index from ?spindle=N, defaulting to 0. */
static bool spindle_from_query(httpd_req_t *req, uint8_t *out)
{
    uint32_t v = 0;
    if (!query_uint(req, "spindle", &v)) v = 0;
    if (v >= NUM_SPINDLES) return false;
    *out = (uint8_t)v;
    return true;
}

/* Assign only if the key is present and numeric — absent keys leave the
 * working copy untouched, so a partial payload is a partial update. */
static void set_float(cJSON *obj, const char *key, float *dst)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) *dst = (float)it->valuedouble;
}

static void set_u16(cJSON *obj, const char *key, uint16_t *dst)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) *dst = (uint16_t)it->valuedouble;
}

static void set_u8(cJSON *obj, const char *key, uint8_t *dst)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) *dst = (uint8_t)it->valueint;
}

static void set_bool(cJSON *obj, const char *key, bool *dst)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it)) *dst = cJSON_IsTrue(it);
}

/* Commit a working copy and report the outcome as JSON. */
static esp_err_t commit_and_reply(httpd_req_t *req, app_config_t *working,
                                  bool reconfigure_comms)
{
    cfg_result_t reason = CFG_OK;
    esp_err_t err = config_store_commit(working, &reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config rejected: %s", app_config_result_str(reason));
        return send_json_error(req, app_config_result_str(reason));
    }

    const app_config_t *live = config_get();
    if (reconfigure_comms) {
        wifi_reconfigure(live);
        modbus_reconfigure(live);
    }
    monitor_config_changed();

    return send_status_ok(req);
}

/* ============================================================
 * GET /
 * ============================================================ */

/* The page is stored pre-gzipped on the web partition (main/CMakeLists.txt
 * gzips it once at build time — see WEB_FS_MOUNT's mount_web_fs() below).
 * This always serves the compressed bytes with Content-Encoding: gzip
 * rather than negotiating against Accept-Encoding: there is no
 * uncompressed copy stored to fall back to, and every browser this page
 * will ever be opened in sends `Accept-Encoding: gzip` unconditionally. */
static esp_err_t handle_index(httpd_req_t *req)
{
    FILE *f = fopen(WEB_FS_MOUNT "/index.html.gz", "rb");
    if (!f) {
        ESP_LOGE(TAG, "index.html.gz missing from the web partition");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char buf[WEB_FS_CHUNK_LEN];
    esp_err_t err = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    fclose(f);

    if (err == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    } else {
        ESP_LOGE(TAG, "failed sending index.html.gz");
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
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
    cJSON_AddNumberToObject(root, "worst_loop_ms", snap.worst_loop_ms);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(snap.uptime_us / 1000000));

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
        cJSON_AddStringToObject(s, "pressure_unit", cfg->spindle[i].pressure.unit);
        cJSON_AddNumberToObject(s, "rpm", sp->rpm);

        cJSON_AddStringToObject(s, "severity", severity_str(sp->severity));
        cJSON_AddBoolToObject(s, "any_alarm", sp->any_alarm);
        cJSON_AddBoolToObject(s, "any_warning", sp->any_warning);
        cJSON_AddBoolToObject(s, "any_latched", sp->any_latched);
        cJSON_AddBoolToObject(s, "breakage", sp->breakage);
        cJSON_AddBoolToObject(s, "crash", sp->crash);
        cJSON_AddBoolToObject(s, "trend", sp->trend);

        cJSON_AddStringToObject(s, "current_status",
                                sensor_status_str(sp->current_status));
        cJSON_AddStringToObject(s, "pressure_status",
                                sensor_status_str(sp->pressure_status));
        cJSON_AddBoolToObject(s, "rpm_sensor_suspect", sp->rpm_sensor_suspect);

        /* Per-quantity band bitmaps, bit0..3 = LoLo/Lo/Hi/HiHi — matching
         * shared/smu_proto.h's SMU_BAND_*_BIT exactly, so a client that
         * already knows that convention (e.g. from the Modbus map) reads
         * these identically. Named "bands_active_current" etc rather than
         * nested under "bands":{"current":...} to keep the common case
         * (does anything need attention right now) a single flat lookup;
         * the thresholds editor, which needs the full per-band shape, gets
         * it from /api/thresholds instead, not from here. */
        cJSON_AddNumberToObject(s, "bands_active_current",   sp->bands_active[QTY_CURRENT]);
        cJSON_AddNumberToObject(s, "bands_active_pressure",  sp->bands_active[QTY_PRESSURE]);
        cJSON_AddNumberToObject(s, "bands_active_rpm",       sp->bands_active[QTY_RPM]);
        cJSON_AddNumberToObject(s, "bands_latched_current",  sp->bands_latched[QTY_CURRENT]);
        cJSON_AddNumberToObject(s, "bands_latched_pressure", sp->bands_latched[QTY_PRESSURE]);
        cJSON_AddNumberToObject(s, "bands_latched_rpm",      sp->bands_latched[QTY_RPM]);

        cJSON_AddNumberToObject(s, "output_state", sp->output_state);
        cJSON_AddNumberToObject(s, "diag_flags", sp->diag_flags);

        cJSON_AddNumberToObject(s, "cycle_count", sp->cycle_count);
        cJSON_AddNumberToObject(s, "last_cycle_ms", sp->last_cycle_ms);
        cJSON_AddNumberToObject(s, "last_cycle_mean_a", sp->last_cycle_mean_a);
        cJSON_AddNumberToObject(s, "last_cycle_peak_a", sp->last_cycle_peak_a);

        /* Per-SMU link health — without this an operator cannot tell a
         * working system from a blind one (FIRMWARE_DESIGN_SPEC.md §5.4).
         * Nested, unlike the flat fields above: this describes the LINK,
         * not the process, and keeping that boundary visible in the JSON
         * shape mirrors the same separation the Modbus map enforces
         * between its link block and its telemetry block. */
        const smu_link_status_t *ls = &snap.link[i];
        cJSON *link = cJSON_AddObjectToObject(s, "link");
        cJSON_AddStringToObject(link, "state", link_state_str(ls->state));
        cJSON_AddBoolToObject(link, "have_ever_linked", ls->have_ever_linked);
        cJSON_AddNumberToObject(link, "age_ms", ls->age_ms);
        cJSON_AddNumberToObject(link, "crc_error_count", ls->crc_error_count);
        cJSON_AddNumberToObject(link, "timeout_count", ls->timeout_count);
        cJSON_AddNumberToObject(link, "poll_count", ls->poll_count);

        cJSON_AddItemToArray(spindles, s);
    }

    return send_json(req, root);
}

/* ============================================================
 * GET /api/smu — per-SMU identity and link health, standalone from the
 * high-rate /api/telemetry poll so a status panel can refresh this at a
 * slower cadence without re-fetching everything else.
 * ============================================================ */

static esp_err_t handle_smu_get(httpd_req_t *req)
{
    monitor_snapshot_t snap;
    monitor_get_snapshot(&snap);
    const app_config_t *cfg = config_get();

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "smus");

    for (int i = 0; i < NUM_SPINDLES; i++) {
        const smu_link_status_t *ls = &snap.link[i];
        cJSON *m = cJSON_CreateObject();

        cJSON_AddNumberToObject(m, "spindle", i);
        cJSON_AddStringToObject(m, "name", cfg->spindle[i].name);
        cJSON_AddStringToObject(m, "link_state", link_state_str(ls->state));
        cJSON_AddBoolToObject(m, "have_ever_linked", ls->have_ever_linked);
        cJSON_AddNumberToObject(m, "age_ms", ls->age_ms);
        cJSON_AddNumberToObject(m, "crc_error_count", ls->crc_error_count);
        cJSON_AddNumberToObject(m, "timeout_count", ls->timeout_count);
        cJSON_AddNumberToObject(m, "poll_count", ls->poll_count);

        /* fw_version/reset_cause/reset_count/smu_uptime_ms only mean
         * anything once identity has actually been read at least once —
         * a link that has never gone live has zeroed, not meaningful,
         * ident fields (monitor.c's build_link_status() leaves them at
         * their memset default until ident_valid). have_ever_linked
         * above is what a client should gate display on. */
        char fw[16];
        snprintf(fw, sizeof(fw), "%u.%u",
                (unsigned)(ls->fw_version >> 8), (unsigned)(ls->fw_version & 0xFF));
        cJSON_AddStringToObject(m, "fw_version", fw);
        cJSON_AddStringToObject(m, "reset_cause", reset_cause_str(ls->reset_cause));
        cJSON_AddNumberToObject(m, "reset_count", ls->reset_count);
        cJSON_AddNumberToObject(m, "smu_uptime_s", ls->smu_uptime_ms / 1000);

        /* config_synced: does the SMU's last-reported schema version match
         * what this build always pushes (CONFIG_SCHEMA_VERSION)? False
         * doesn't distinguish "push still in flight" from "never
         * acknowledged" — either way the operator's answer is the same:
         * don't trust this SMU's thresholds are current yet. */
        cJSON_AddNumberToObject(m, "cfg_schema_version", ls->cfg_schema_version);
        cJSON_AddBoolToObject(m, "config_synced",
            ls->have_ever_linked && ls->cfg_schema_version == CONFIG_SCHEMA_VERSION);

        cJSON_AddItemToArray(arr, m);
    }

    return send_json(req, root);
}

/* ============================================================
 * GET /api/history?spindle=N
 * ============================================================ */

static esp_err_t handle_history_get(httpd_req_t *req)
{
    uint8_t spindle;
    if (!spindle_from_query(req, &spindle)) {
        return send_json_error(req, "bad spindle index");
    }

    trend_sample_t *samples = malloc(sizeof(trend_sample_t) * TREND_CAPACITY);
    if (!samples) return send_json_error(req, "out of memory");

    uint32_t age_ms = 0;
    size_t n = trend_get(spindle, samples, TREND_CAPACITY, &age_ms);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "spindle", spindle);
    cJSON_AddNumberToObject(root, "period_ms", TREND_PERIOD_MS);
    cJSON_AddNumberToObject(root, "age_ms", age_ms);

    /* Three parallel arrays rather than an array of objects: the payload is
     * roughly a third the size for 300 points, which matters over WiFi at
     * this refresh rate. */
    cJSON *cur = cJSON_AddArrayToObject(root, "current_a");
    cJSON *pre = cJSON_AddArrayToObject(root, "pressure");
    cJSON *rpm = cJSON_AddArrayToObject(root, "rpm");

    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(cur, cJSON_CreateNumber(samples[i].current_a));
        cJSON_AddItemToArray(pre, cJSON_CreateNumber(samples[i].pressure));
        cJSON_AddItemToArray(rpm, cJSON_CreateNumber(samples[i].rpm));
    }

    free(samples);
    return send_json(req, root);
}

/* ============================================================
 * GET /api/system
 * ============================================================ */

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_BROWNOUT: return "brown-out";
    case ESP_RST_DEEPSLEEP:return "deep sleep wake";
    default:               return "other";
    }
}

static esp_err_t handle_system_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "project", app->project_name);
    cJSON_AddStringToObject(root, "build_date", app->date);
    cJSON_AddStringToObject(root, "build_time", app->time);
    cJSON_AddStringToObject(root, "idf_version", app->idf_ver);
    cJSON_AddNumberToObject(root, "schema_version", CONFIG_SCHEMA_VERSION);

    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "reset_reason", reset_reason_str());

    cJSON_AddStringToObject(root, "running_partition",
                            running ? running->label : "?");
    cJSON_AddStringToObject(root, "update_partition", next ? next->label : "none");

    /* Whether this build is still on probation. If it is, a reboot before
     * the self-confirm window elapses reverts to the previous image. */
    esp_ota_img_states_t state;
    bool pending = running &&
                   esp_ota_get_state_partition(running, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;
    cJSON_AddBoolToObject(root, "pending_verify", pending);

    ota_status_t ota;
    ota_get_status(&ota);
    cJSON *o = cJSON_AddObjectToObject(root, "ota");
    cJSON_AddBoolToObject(o, "in_progress", ota.in_progress);
    cJSON_AddNumberToObject(o, "received", ota.received);
    cJSON_AddNumberToObject(o, "total", ota.total);

    return send_json(req, root);
}

/* ============================================================
 * GET / POST /api/config   (WiFi + Modbus)
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

    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    cJSON_AddNumberToObject(sys, "measure_period_ms", cfg->system.measure_period_ms);
    cJSON_AddNumberToObject(sys, "mains_hz", cfg->system.mains_hz);

    return send_json(req, root);
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return ESP_OK;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_json_error(req, "invalid JSON");

    app_config_t working;
    config_get_copy(&working);

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(wifi, "ssid");
        if (cJSON_IsString(ssid)) {
            str_copy_bounded(working.system.wifi.ssid, CFG_WIFI_SSID_LEN,
                             ssid->valuestring);
        }
        /* Password key is only sent by the page when the operator typed a
         * new one, so its absence here means "keep the current password" —
         * working already holds it via config_get_copy(). */
        cJSON *pass = cJSON_GetObjectItemCaseSensitive(wifi, "password");
        if (cJSON_IsString(pass)) {
            str_copy_bounded(working.system.wifi.password, CFG_WIFI_PASS_LEN,
                             pass->valuestring);
        }
    }

    cJSON *mb = cJSON_GetObjectItemCaseSensitive(root, "modbus");
    if (cJSON_IsObject(mb)) {
        modbus_cfg_t *m = &working.system.modbus;
        set_bool(mb, "rtu_enabled", &m->rtu_enabled);
        set_bool(mb, "tcp_enabled", &m->tcp_enabled);
        set_u8(mb, "stop_bits", &m->stop_bits);
        set_u8(mb, "data_bits", &m->data_bits);
        set_u8(mb, "slave_id", &m->slave_id);
        set_u16(mb, "tcp_port", &m->tcp_port);

        cJSON *it = cJSON_GetObjectItemCaseSensitive(mb, "baud");
        if (cJSON_IsNumber(it)) m->baud = (uint32_t)it->valuedouble;
        it = cJSON_GetObjectItemCaseSensitive(mb, "parity");
        if (cJSON_IsNumber(it)) m->parity = (cfg_parity_t)it->valueint;
    }

    cJSON *sys = cJSON_GetObjectItemCaseSensitive(root, "system");
    if (cJSON_IsObject(sys)) {
        set_u16(sys, "measure_period_ms", &working.system.measure_period_ms);
        set_u8(sys, "mains_hz", &working.system.mains_hz);
    }

    cJSON_Delete(root);
    return commit_and_reply(req, &working, true);
}

/* ============================================================
 * GET /api/spindle?spindle=N
 * ============================================================ */

static void add_band(cJSON *arr, const band_cfg_t *b)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", b->enabled);
    cJSON_AddNumberToObject(o, "limit", b->limit);
    cJSON_AddNumberToObject(o, "hysteresis", b->hysteresis);
    cJSON_AddNumberToObject(o, "on_delay_ms", b->on_delay_ms);
    cJSON_AddNumberToObject(o, "off_delay_ms", b->off_delay_ms);
    cJSON_AddBoolToObject(o, "latching", b->latching);
    cJSON_AddItemToArray(arr, o);
}

static esp_err_t handle_spindle_get(httpd_req_t *req)
{
    uint8_t i;
    if (!spindle_from_query(req, &i)) {
        return send_json_error(req, "bad spindle index");
    }

    const spindle_cfg_t *s = &config_get()->spindle[i];

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "spindle", i);
    cJSON_AddStringToObject(root, "name", s->name);
    cJSON_AddBoolToObject(root, "enabled", s->enabled);

    cJSON *cur = cJSON_AddObjectToObject(root, "current");
    cJSON_AddNumberToObject(cur, "ct_primary_amps", s->current.ct_primary_amps);
    cJSON_AddNumberToObject(cur, "ct_secondary_ma", s->current.ct_secondary_ma);
    cJSON_AddNumberToObject(cur, "gain_correction", s->current.gain_correction);
    cJSON_AddNumberToObject(cur, "zero_offset_v", s->current.zero_offset_v);
    cJSON_AddNumberToObject(cur, "noload_cutoff_a", s->current.noload_cutoff_a);
    cJSON_AddNumberToObject(cur, "rms_burst_samples", s->current.rms_burst_samples);

    cJSON *pre = cJSON_AddObjectToObject(root, "pressure");
    cJSON_AddNumberToObject(pre, "mode", s->pressure.mode);
    cJSON_AddNumberToObject(pre, "sensor_min", s->pressure.sensor_min);
    cJSON_AddNumberToObject(pre, "sensor_max", s->pressure.sensor_max);
    cJSON_AddStringToObject(pre, "unit", s->pressure.unit);
    cJSON_AddNumberToObject(pre, "gain_correction", s->pressure.gain_correction);
    cJSON_AddNumberToObject(pre, "offset_correction", s->pressure.offset_correction);

    cJSON *rpm = cJSON_AddObjectToObject(root, "rpm");
    cJSON_AddNumberToObject(rpm, "pulses_per_rev", s->rpm.pulses_per_rev);
    cJSON_AddNumberToObject(rpm, "glitch_filter_ns", s->rpm.glitch_filter_ns);
    cJSON_AddNumberToObject(rpm, "zero_timeout_ms", s->rpm.zero_timeout_ms);

    cJSON *sm = cJSON_AddObjectToObject(root, "sm");
    cJSON_AddNumberToObject(sm, "start_rpm", s->sm.start_rpm);
    cJSON_AddNumberToObject(sm, "settle_ms", s->sm.settle_ms);
    cJSON_AddNumberToObject(sm, "idle_current_a", s->sm.idle_current_a);
    cJSON_AddNumberToObject(sm, "cut_detect_current_a", s->sm.cut_detect_current_a);
    cJSON_AddNumberToObject(sm, "alarm_inhibit_ms", s->sm.alarm_inhibit_ms);

    /* bands[quantity][band], emitted as named quantities so the UI does not
     * have to know the enum ordering. */
    cJSON *bands = cJSON_AddObjectToObject(root, "bands");
    for (int q = 0; q < QTY_COUNT; q++) {
        cJSON *arr = cJSON_AddArrayToObject(bands, quantity_str((quantity_t)q));
        for (int b = 0; b < BAND_COUNT; b++) {
            add_band(arr, &s->bands[q][b]);
        }
    }

    return send_json(req, root);
}

/* ============================================================
 * POST /api/thresholds?spindle=N
 * ============================================================ */

static esp_err_t handle_thresholds_post(httpd_req_t *req)
{
    uint8_t i;
    if (!spindle_from_query(req, &i)) {
        return send_json_error(req, "bad spindle index");
    }

    char *body = read_body(req);
    if (!body) return ESP_OK;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_json_error(req, "invalid JSON");

    app_config_t working;
    config_get_copy(&working);
    spindle_cfg_t *s = &working.spindle[i];

    cJSON *bands = cJSON_GetObjectItemCaseSensitive(root, "bands");
    if (cJSON_IsObject(bands)) {
        for (int q = 0; q < QTY_COUNT; q++) {
            cJSON *arr = cJSON_GetObjectItemCaseSensitive(
                bands, quantity_str((quantity_t)q));
            if (!cJSON_IsArray(arr)) continue;

            for (int b = 0; b < BAND_COUNT; b++) {
                cJSON *o = cJSON_GetArrayItem(arr, b);
                if (!cJSON_IsObject(o)) continue;

                band_cfg_t *dst = &s->bands[q][b];
                set_bool(o, "enabled", &dst->enabled);
                set_float(o, "limit", &dst->limit);
                set_float(o, "hysteresis", &dst->hysteresis);
                set_u16(o, "on_delay_ms", &dst->on_delay_ms);
                set_u16(o, "off_delay_ms", &dst->off_delay_ms);
                set_bool(o, "latching", &dst->latching);
            }
        }
    }

    cJSON *sm = cJSON_GetObjectItemCaseSensitive(root, "sm");
    if (cJSON_IsObject(sm)) {
        set_float(sm, "start_rpm", &s->sm.start_rpm);
        set_u16(sm, "settle_ms", &s->sm.settle_ms);
        set_float(sm, "idle_current_a", &s->sm.idle_current_a);
        set_float(sm, "cut_detect_current_a", &s->sm.cut_detect_current_a);
        set_u16(sm, "alarm_inhibit_ms", &s->sm.alarm_inhibit_ms);
    }

    set_bool(root, "enabled", &s->enabled);

    cJSON_Delete(root);
    return commit_and_reply(req, &working, false);
}

/* ============================================================
 * POST /api/calibration?spindle=N   (sensor setup and raw trims)
 * ============================================================ */

static esp_err_t handle_calibration_post(httpd_req_t *req)
{
    uint8_t i;
    if (!spindle_from_query(req, &i)) {
        return send_json_error(req, "bad spindle index");
    }

    char *body = read_body(req);
    if (!body) return ESP_OK;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_json_error(req, "invalid JSON");

    app_config_t working;
    config_get_copy(&working);
    spindle_cfg_t *s = &working.spindle[i];

    cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (cJSON_IsObject(o)) {
        set_float(o, "ct_primary_amps", &s->current.ct_primary_amps);
        set_float(o, "ct_secondary_ma", &s->current.ct_secondary_ma);
        set_float(o, "gain_correction", &s->current.gain_correction);
        set_float(o, "noload_cutoff_a", &s->current.noload_cutoff_a);
        set_u16(o, "rms_burst_samples", &s->current.rms_burst_samples);
    }

    o = cJSON_GetObjectItemCaseSensitive(root, "pressure");
    if (cJSON_IsObject(o)) {
        set_float(o, "sensor_min", &s->pressure.sensor_min);
        set_float(o, "sensor_max", &s->pressure.sensor_max);
        set_float(o, "gain_correction", &s->pressure.gain_correction);
        set_float(o, "offset_correction", &s->pressure.offset_correction);

        cJSON *mode = cJSON_GetObjectItemCaseSensitive(o, "mode");
        if (cJSON_IsNumber(mode)) {
            s->pressure.mode = (pressure_input_mode_t)mode->valueint;
        }
        cJSON *unit = cJSON_GetObjectItemCaseSensitive(o, "unit");
        if (cJSON_IsString(unit)) {
            str_copy_bounded(s->pressure.unit, CFG_UNIT_LEN, unit->valuestring);
        }
    }

    o = cJSON_GetObjectItemCaseSensitive(root, "rpm");
    if (cJSON_IsObject(o)) {
        set_u16(o, "pulses_per_rev", &s->rpm.pulses_per_rev);
        set_u16(o, "glitch_filter_ns", &s->rpm.glitch_filter_ns);
        set_u16(o, "zero_timeout_ms", &s->rpm.zero_timeout_ms);
    }

    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name)) {
        str_copy_bounded(s->name, CFG_NAME_LEN, name->valuestring);
    }

    set_bool(root, "enabled", &s->enabled);

    cJSON_Delete(root);
    return commit_and_reply(req, &working, false);
}

/* ============================================================
 * GET / POST /api/calib   (guided wizard)
 * ============================================================ */

static esp_err_t handle_calib_get(httpd_req_t *req)
{
    uint8_t i;
    if (!spindle_from_query(req, &i)) {
        return send_json_error(req, "bad spindle index");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "spindle", i);

    static const char *names[CALIB_TARGET_COUNT] = { "current", "pressure" };

    for (int t = 0; t < CALIB_TARGET_COUNT; t++) {
        calib_state_t st;
        calib_get_state(i, (calib_target_t)t, &st);

        cJSON *o = cJSON_AddObjectToObject(root, names[t]);
        cJSON_AddNumberToObject(o, "live_nominal", st.live_nominal);
        cJSON_AddNumberToObject(o, "live_corrected", st.live_corrected);
        cJSON_AddNumberToObject(o, "raw", st.raw);

        cJSON *pts = cJSON_AddArrayToObject(o, "points");
        for (int p = 0; p < CALIB_MAX_POINTS; p++) {
            cJSON *pt = cJSON_CreateObject();
            cJSON_AddBoolToObject(pt, "captured", st.point[p].captured);
            cJSON_AddNumberToObject(pt, "nominal", st.point[p].nominal);
            cJSON_AddNumberToObject(pt, "reference", st.point[p].reference);
            cJSON_AddItemToArray(pts, pt);
        }
    }

    return send_json(req, root);
}

static esp_err_t handle_calib_post(httpd_req_t *req)
{
    uint8_t i;
    if (!spindle_from_query(req, &i)) {
        return send_json_error(req, "bad spindle index");
    }

    char *body = read_body(req);
    if (!body) return ESP_OK;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_json_error(req, "invalid JSON");

    cJSON *j_target = cJSON_GetObjectItemCaseSensitive(root, "target");
    cJSON *j_action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(j_target) || !cJSON_IsString(j_action)) {
        cJSON_Delete(root);
        return send_json_error(req, "target and action are required");
    }

    calib_target_t target = (strcmp(j_target->valuestring, "pressure") == 0)
                          ? CALIB_PRESSURE : CALIB_CURRENT;
    const char *action = j_action->valuestring;

    esp_err_t err = ESP_OK;
    cfg_result_t reason = CFG_OK;

    if (strcmp(action, "capture") == 0) {
        cJSON *j_ref = cJSON_GetObjectItemCaseSensitive(root, "reference");
        cJSON *j_idx = cJSON_GetObjectItemCaseSensitive(root, "index");
        if (!cJSON_IsNumber(j_ref)) {
            cJSON_Delete(root);
            return send_json_error(req, "reference value is required");
        }
        uint8_t index = cJSON_IsNumber(j_idx) ? (uint8_t)j_idx->valueint : 0;
        err = calib_capture(i, target, index, (float)j_ref->valuedouble);

    } else if (strcmp(action, "apply") == 0) {
        err = calib_apply(i, target, &reason);

    } else if (strcmp(action, "reset") == 0) {
        calib_reset(i, target);

    } else {
        cJSON_Delete(root);
        return send_json_error(req, "unknown action");
    }

    cJSON_Delete(root);

    if (err != ESP_OK) {
        if (reason != CFG_OK) return send_json_error(req, app_config_result_str(reason));
        if (err == ESP_ERR_INVALID_STATE) {
            return send_json_error(req,
                "not enough usable calibration points — capture two points "
                "far enough apart, at a real load");
        }
        return send_json_error(req, esp_err_to_name(err));
    }

    monitor_config_changed();
    return send_status_ok(req);
}

/* ============================================================
 * POST /api/autozero?spindle=N
 * ============================================================ */

static esp_err_t handle_autozero_post(httpd_req_t *req)
{
    uint8_t i;
    if (!spindle_from_query(req, &i)) {
        return send_json_error(req, "bad spindle index");
    }

    esp_err_t err = calib_autozero(i);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_RESPONSE) {
            return send_json_error(req,
                "measured bias is far from the expected value — check the CT "
                "is connected and the spindle really is stopped");
        }
        return send_json_error(req, esp_err_to_name(err));
    }
    return send_status_ok(req);
}

/* ============================================================
 * POST /api/acknowledge?spindle=N   (omit spindle to ack all)
 * ============================================================ */

static esp_err_t handle_acknowledge_post(httpd_req_t *req)
{
    uint32_t v;
    /* No parameter means every spindle: monitor_acknowledge() treats an
     * out-of-range index as "all". */
    uint8_t spindle = query_uint(req, "spindle", &v) ? (uint8_t)v : 0xFF;

    monitor_acknowledge(spindle);
    ESP_LOGI(TAG, "alarms acknowledged (spindle %u)", spindle);
    return send_status_ok(req);
}

/* ============================================================
 * POST /api/ota
 * ============================================================ */

static void reboot_task(void *arg)
{
    (void)arg;
    /* Long enough for the HTTP response to reach the browser and the
     * socket to close cleanly. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "restarting");
    esp_restart();
}

static void schedule_reboot(void)
{
    xTaskCreatePinnedToCore(reboot_task, "reboot", 2048, NULL, 5, NULL,
                            CORE_NETWORK);
}

static esp_err_t handle_ota_post(httpd_req_t *req)
{
    ESP_LOGW(TAG, "firmware upload starting (%d bytes)", req->content_len);

    esp_err_t err = ota_begin((size_t)req->content_len);
    if (err != ESP_OK) {
        return send_json_error(req, esp_err_to_name(err));
    }

    char *chunk = malloc(OTA_CHUNK_LEN);
    if (!chunk) {
        ota_abort();
        return send_json_error(req, "out of memory");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int want = (remaining < OTA_CHUNK_LEN) ? remaining : OTA_CHUNK_LEN;
        int r = httpd_req_recv(req, chunk, want);

        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(chunk);
            ota_abort();
            return send_json_error(req, "upload interrupted");
        }

        err = ota_write(chunk, (size_t)r);
        if (err != ESP_OK) {
            free(chunk);
            /* ota_write has already aborted the session. */
            return send_json_error(req,
                (err == ESP_ERR_INVALID_VERSION)
                    ? "that image is for a different project"
                    : "image rejected");
        }
        remaining -= r;
    }

    free(chunk);

    err = ota_end();
    if (err != ESP_OK) {
        return send_json_error(req, "image failed validation — not applied");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "message", "update staged, rebooting");
    esp_err_t sent = send_json(req, root);

    schedule_reboot();
    return sent;
}

/* ============================================================
 * POST /api/reboot, POST /api/factory-reset
 * ============================================================ */

static esp_err_t handle_reboot_post(httpd_req_t *req)
{
    esp_err_t sent = send_status_ok(req);
    schedule_reboot();
    return sent;
}

static esp_err_t handle_factory_reset_post(httpd_req_t *req)
{
    esp_err_t err = config_store_factory_reset();
    if (err != ESP_OK) return send_json_error(req, esp_err_to_name(err));

    ESP_LOGW(TAG, "factory reset — restarting");
    esp_err_t sent = send_status_ok(req);
    schedule_reboot();
    return sent;
}

/* The `web` partition (partitions.csv) is written by `idf.py flash` from
 * the image main/CMakeLists.txt builds — not formatted on-device — so a
 * mount failure means the image is missing or corrupt, not an empty
 * filesystem to paper over. format_if_mount_failed is deliberately false:
 * silently reformatting would erase a UI build the operator flashed but
 * turn a build/flash mistake into "web UI resets itself for no reason". */
static esp_err_t mount_web_fs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = WEB_FS_MOUNT,
        .partition_label = WEB_FS_PARTITION,
        .format_if_mount_failed = false,
        .read_only = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount web partition: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(WEB_FS_PARTITION, &total, &used);
    ESP_LOGI(TAG, "web partition mounted: %u/%u bytes used",
             (unsigned)used, (unsigned)total);
    return ESP_OK;
}

/* ============================================================
 * Job templates
 *
 * Saved sets of job settings an operator can reload in one action, including
 * onto a different machine. The rules about what may travel (and what must
 * never) are job_template.[ch]'s; storage is job_store.[ch]'s. This layer is
 * HTTP plumbing plus the two guards that belong at the point of application:
 * refusing to load a job into a spindle that is mid-cut, and surfacing
 * unreachable-band warnings to the operator.
 * ============================================================ */

/* Percent-decode in place. httpd_query_key_value() does NOT decode, so a
 * template named "Acme bracket" arrives as "Acme%20bracket" — which would
 * then fail job_store's filename allowlist on the '%' and look like a
 * mysterious rejection of a perfectly ordinary name. */
static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && isxdigit((unsigned char)r[1]) &&
                                isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* Fetch and decode a string query parameter, e.g. ?name=Acme%20bracket.
 * The query buffer is sized for a full-length name plus the other parameters
 * that can accompany it. */
static bool query_str(httpd_req_t *req, const char *key, char *out, size_t out_size)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, key, out, out_size) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return out[0] != '\0';
}

static cJSON *warnings_to_json(const job_warnings_t *w)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < w->count; i++) {
        const job_warning_t *it = &w->item[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "kind", job_warn_kind_str(it->kind));
        cJSON_AddStringToObject(o, "quantity", quantity_str(it->qty));
        cJSON_AddStringToObject(o, "band", band_str(it->band));
        cJSON_AddNumberToObject(o, "limit", it->limit);
        cJSON_AddNumberToObject(o, "machine_max", it->machine_max);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* GET /api/jobs            — list every saved template
 * GET /api/jobs?name=X     — fetch one, which is also the export path */
static esp_err_t handle_jobs_get(httpd_req_t *req)
{
    if (!job_store_ready()) return send_json_error(req, "job storage unavailable");

    char name[JOB_NAME_MAX];
    if (query_str(req, "name", name, sizeof(name))) {
        job_record_t rec;
        esp_err_t err = job_store_load(name, &rec);
        if (err == ESP_ERR_NOT_FOUND)   return send_json_error(req, "no such template");
        if (err != ESP_OK)              return send_json_error(req, "template unreadable");

        cJSON *root = job_record_to_json(&rec);
        if (!root) return send_json_error(req, "out of memory");
        return send_json(req, root);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "jobs", job_store_list());
    return send_json(req, root);
}

/* POST /api/jobs?name=X — capture both spindles' current settings.
 * Body carries notes/saved_at/saved_by; the browser supplies the timestamp
 * because this device has no RTC. */
static esp_err_t handle_jobs_post(httpd_req_t *req)
{
    if (!job_store_ready()) return send_json_error(req, "job storage unavailable");

    char name[JOB_NAME_MAX];
    if (!query_str(req, "name", name, sizeof(name))) {
        return send_json_error(req, "template name required");
    }

    job_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    str_copy_bounded(meta.name, sizeof(meta.name), name);

    char *body = read_body(req);
    if (body) {
        cJSON *root = cJSON_Parse(body);
        free(body);
        if (root) {
            cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "notes");
            if (cJSON_IsString(it)) str_copy_bounded(meta.notes, sizeof(meta.notes), it->valuestring);
            it = cJSON_GetObjectItemCaseSensitive(root, "saved_at");
            if (cJSON_IsString(it)) str_copy_bounded(meta.saved_at, sizeof(meta.saved_at), it->valuestring);
            it = cJSON_GetObjectItemCaseSensitive(root, "saved_by");
            if (cJSON_IsString(it)) str_copy_bounded(meta.saved_by, sizeof(meta.saved_by), it->valuestring);
            cJSON_Delete(root);
        }
    }

    job_record_t rec;
    job_record_capture(config_get(), &meta, &rec);

    esp_err_t err = job_store_save(&rec);
    if (err == ESP_ERR_INVALID_ARG) return send_json_error(req, "invalid template name");
    if (err != ESP_OK)              return send_json_error(req, "could not save (storage full?)");

    return send_status_ok(req);
}

static esp_err_t handle_jobs_delete(httpd_req_t *req)
{
    if (!job_store_ready()) return send_json_error(req, "job storage unavailable");

    char name[JOB_NAME_MAX];
    if (!query_str(req, "name", name, sizeof(name))) {
        return send_json_error(req, "template name required");
    }

    esp_err_t err = job_store_delete(name);
    if (err == ESP_ERR_INVALID_ARG) return send_json_error(req, "invalid template name");
    if (err != ESP_OK)              return send_json_error(req, "no such template");

    return send_status_ok(req);
}

/* POST /api/jobs/import — accept a template file exported from another
 * machine and store it. Validation against THIS machine happens at apply
 * time, not here: a template can legitimately be kept on a machine it does
 * not currently fit (a different sensor could be fitted later), and refusing
 * the import would lose the file entirely. */
static esp_err_t handle_jobs_import_post(httpd_req_t *req)
{
    if (!job_store_ready()) return send_json_error(req, "job storage unavailable");

    char *body = read_body(req);
    if (!body) return ESP_OK;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_json_error(req, "invalid JSON");

    job_record_t rec;
    esp_err_t err = job_record_from_json(root, &rec);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_json_error(req, "not a job template file");
    }

    /* An imported file may carry a name that would not be a legal filename
     * here, or none at all — say so plainly rather than writing something
     * the operator did not choose. */
    if (rec.meta.name[0] == '\0') {
        return send_json_error(req, "template file has no name");
    }

    err = job_store_save(&rec);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json_error(req, "template name has characters this device cannot store");
    }
    if (err != ESP_OK) return send_json_error(req, "could not save (storage full?)");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "name", rec.meta.name);
    return send_json(req, resp);
}

/* Shared tail for both apply and copy: guard, check, apply, commit, reply.
 * `ctx` is NULL for a same-machine copy — see job_check(). */
static esp_err_t apply_profile_to_spindle(httpd_req_t *req,
                                          const job_profile_t *profile,
                                          const job_context_t *ctx,
                                          uint16_t schema_version,
                                          uint8_t target)
{
    /* Guard 1: never swap a job into a spindle that is mid-cut. Changing
     * every threshold under a running tool is almost always a mistake, and
     * unlike a single threshold tweak there is no plausible reason to do it
     * without stopping first. This also underwrites the decision to let
     * unreachable-band warnings through rather than block on them: because
     * apply only happens on a stopped spindle, a human is always present at
     * the machine when a warning appears. */
    monitor_snapshot_t snap;
    monitor_get_snapshot(&snap);
    if (snap.spindle[target].state == SPINDLE_CUTTING) {
        return send_json_error(req,
            "spindle is cutting — stop it before loading a job");
    }

    const app_config_t *live = config_get();

    /* Guard 2: compatibility with THIS machine's hardware. */
    job_warnings_t warn;
    job_check_result_t chk = job_check(profile, ctx, &live->spindle[target],
                                       schema_version, CONFIG_SCHEMA_VERSION,
                                       &warn);
    if (chk != JOB_OK) {
        return send_json_error(req, job_check_result_str(chk));
    }

    app_config_t working;
    config_get_copy(&working);
    job_profile_apply(profile, &working.spindle[target]);

    cfg_result_t reason = CFG_OK;
    esp_err_t err = config_store_commit(&working, &reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "job apply rejected: %s", app_config_result_str(reason));
        return send_json_error(req, app_config_result_str(reason));
    }

    /* Same propagation path every other config write uses — this is what
     * carries the new thresholds down to the SMU. */
    monitor_config_changed();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddItemToObject(resp, "warnings", warnings_to_json(&warn));
    cJSON_AddBoolToObject(resp, "warnings_truncated", warn.truncated);
    return send_json(req, resp);
}

/* POST /api/jobs/apply?name=X&spindle=N[&from=M]
 *
 * `from` selects which of the template's spindle profiles to use, so an
 * operation authored on spindle 0 can be applied to spindle 1 when the same
 * job moves sides. Omitted, each profile goes to its own index. */
static esp_err_t handle_jobs_apply_post(httpd_req_t *req)
{
    if (!job_store_ready()) return send_json_error(req, "job storage unavailable");

    char name[JOB_NAME_MAX];
    if (!query_str(req, "name", name, sizeof(name))) {
        return send_json_error(req, "template name required");
    }

    uint8_t target;
    if (!spindle_from_query(req, &target)) {
        return send_json_error(req, "bad spindle index");
    }

    job_record_t rec;
    esp_err_t err = job_store_load(name, &rec);
    if (err == ESP_ERR_NOT_FOUND) return send_json_error(req, "no such template");
    if (err != ESP_OK)            return send_json_error(req, "template unreadable");

    uint32_t from = target;
    (void)query_uint(req, "from", &from);
    if (from >= NUM_SPINDLES)  return send_json_error(req, "bad source spindle");
    if (!rec.present[from])    return send_json_error(req,
                                   "this template has no settings for that spindle");

    return apply_profile_to_spindle(req, &rec.profile[from], &rec.context[from],
                                    rec.schema_version, target);
}

/* POST /api/jobs/copy?from=N&to=M
 *
 * For when both spindles run the same kind of job. Deliberately routed
 * through the same extract/check/apply path as a template rather than a
 * struct copy: spindle 2 has its own CT and its own pressure sensor with
 * their own gain corrections, so copying the whole spindle_cfg_t would skew
 * every reading on that side — the identical silent-wrongness bug as a
 * cross-machine import, just inside one machine. */
static esp_err_t handle_jobs_copy_post(httpd_req_t *req)
{
    uint32_t from = 0, to = 0;
    if (!query_uint(req, "from", &from) || !query_uint(req, "to", &to)) {
        return send_json_error(req, "from and to spindle required");
    }
    if (from >= NUM_SPINDLES || to >= NUM_SPINDLES) {
        return send_json_error(req, "bad spindle index");
    }
    if (from == to) {
        return send_json_error(req, "source and target are the same spindle");
    }

    const app_config_t *live = config_get();
    job_profile_t profile;
    job_profile_extract(&live->spindle[from], &profile);

    /* NULL context: both spindles are on one machine and share a pressure
     * unit by construction, so there is nothing to cross-check. Reachability
     * warnings are still produced, which matters when the two sides have
     * different CT ratings or sensor spans. */
    return apply_profile_to_spindle(req, &profile, NULL,
                                    CONFIG_SCHEMA_VERSION, (uint8_t)to);
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t web_init(const app_config_t *cfg)
{
    (void)cfg;

    esp_err_t fs_err = mount_web_fs();
    if (fs_err != ESP_OK) return fs_err;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size      = STACK_WEB;
    config.core_id         = CORE_NETWORK;
    /* Default is 8 and we register far more than that; without this the
     * extra routes fail to register and return 404 at runtime — a failure
     * that does not show up at build time, only as a mysteriously missing
     * endpoint. The static assert below is what makes that impossible to
     * reintroduce: adding a route without raising this number now breaks the
     * build instead of one API call.
     *
     * 32 rather than exactly the current count so ordinary growth does not
     * require touching this line; each slot is a small struct in the httpd
     * control block, not a per-connection cost. */
    config.max_uri_handlers = 32;
    /* A firmware upload is ~1 MB over WiFi and can stall briefly; the
     * default 5 s would abort a perfectly good update. */
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;
    config.lru_purge_enable  = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = handle_index },
        { .uri = "/api/telemetry",     .method = HTTP_GET,  .handler = handle_telemetry_get },
        { .uri = "/api/smu",           .method = HTTP_GET,  .handler = handle_smu_get },
        { .uri = "/api/history",       .method = HTTP_GET,  .handler = handle_history_get },
        { .uri = "/api/system",        .method = HTTP_GET,  .handler = handle_system_get },
        { .uri = "/api/config",        .method = HTTP_GET,  .handler = handle_config_get },
        { .uri = "/api/config",        .method = HTTP_POST, .handler = handle_config_post },
        { .uri = "/api/spindle",       .method = HTTP_GET,  .handler = handle_spindle_get },
        { .uri = "/api/thresholds",    .method = HTTP_POST, .handler = handle_thresholds_post },
        { .uri = "/api/calibration",   .method = HTTP_POST, .handler = handle_calibration_post },
        { .uri = "/api/calib",         .method = HTTP_GET,  .handler = handle_calib_get },
        { .uri = "/api/calib",         .method = HTTP_POST, .handler = handle_calib_post },
        { .uri = "/api/autozero",      .method = HTTP_POST, .handler = handle_autozero_post },
        { .uri = "/api/acknowledge",   .method = HTTP_POST, .handler = handle_acknowledge_post },
        { .uri = "/api/jobs",          .method = HTTP_GET,    .handler = handle_jobs_get },
        { .uri = "/api/jobs",          .method = HTTP_POST,   .handler = handle_jobs_post },
        { .uri = "/api/jobs",          .method = HTTP_DELETE, .handler = handle_jobs_delete },
        { .uri = "/api/jobs/apply",    .method = HTTP_POST,   .handler = handle_jobs_apply_post },
        { .uri = "/api/jobs/copy",     .method = HTTP_POST,   .handler = handle_jobs_copy_post },
        { .uri = "/api/jobs/import",   .method = HTTP_POST,   .handler = handle_jobs_import_post },
        { .uri = "/api/ota",           .method = HTTP_POST, .handler = handle_ota_post },
        { .uri = "/api/reboot",        .method = HTTP_POST, .handler = handle_reboot_post },
        { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = handle_factory_reset_post },
    };

    /* Registering more routes than max_uri_handlers allows fails at RUNTIME,
     * per-route, as a 404 on whichever endpoints did not fit — it builds
     * cleanly and looks like a missing feature rather than a configuration
     * error. This catches it at compile time instead. */
    _Static_assert(sizeof(routes) / sizeof(routes[0]) <= 32,
                   "more routes than config.max_uri_handlers allows — raise both");

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }

    ESP_LOGI(TAG, "web UI listening on port %d", config.server_port);
    return ESP_OK;
}
