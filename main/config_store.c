/*
 * config_store.c — NVS-backed configuration with known-good fallback.
 */

#include "config_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "config";

#define NVS_NAMESPACE   "cncmon"
#define KEY_ACTIVE      "cfg_active"
#define KEY_KNOWN_GOOD  "cfg_good"

static app_config_t      s_cfg;
static SemaphoreHandle_t s_cfg_mutex;

/* ============================================================
 * Helpers
 * ============================================================ */

static esp_err_t read_slot(const char *key, app_config_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(*out);
    err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);

    if (err != ESP_OK) return err;

    /* A short or long blob means the schema changed under us. Treat it the
     * same as corruption rather than trying to interpret it. */
    if (len != sizeof(*out)) {
        ESP_LOGW(TAG, "slot '%s' size %u, expected %u", key,
                 (unsigned)len, (unsigned)sizeof(*out));
        return ESP_ERR_INVALID_SIZE;
    }
    if (!app_config_check(out)) {
        ESP_LOGW(TAG, "slot '%s' failed CRC/version check", key);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static esp_err_t write_slot(const char *key, const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, key, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t config_store_init(config_load_source_t *source_out)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    s_cfg_mutex = xSemaphoreCreateMutex();
    if (!s_cfg_mutex) return ESP_ERR_NO_MEM;

    config_load_source_t src;

    if (read_slot(KEY_ACTIVE, &s_cfg) == ESP_OK) {
        src = CFG_LOADED_ACTIVE;
    } else if (read_slot(KEY_KNOWN_GOOD, &s_cfg) == ESP_OK) {
        ESP_LOGW(TAG, "active config bad — recovered from known-good");
        src = CFG_LOADED_KNOWN_GOOD;
        /* Repair the active slot so the next boot is clean. */
        (void)write_slot(KEY_ACTIVE, &s_cfg);
    } else {
        ESP_LOGW(TAG, "no usable config — applying factory defaults");
        app_config_set_defaults(&s_cfg);
        src = CFG_LOADED_DEFAULTS;
        (void)write_slot(KEY_ACTIVE, &s_cfg);
        (void)write_slot(KEY_KNOWN_GOOD, &s_cfg);
    }

    /* Defaults are trusted, but a blob recovered from flash is not: an
     * older firmware could have written something this build considers
     * unsafe. Validate before letting it drive outputs. */
    cfg_result_t vr = app_config_validate(&s_cfg, NULL);
    if (vr != CFG_OK) {
        ESP_LOGE(TAG, "stored config invalid (%s) — reverting to defaults",
                 app_config_result_str(vr));
        app_config_set_defaults(&s_cfg);
        src = CFG_LOADED_DEFAULTS;
        (void)write_slot(KEY_ACTIVE, &s_cfg);
    }

    if (source_out) *source_out = src;
    return ESP_OK;
}

const app_config_t *config_get(void)
{
    return &s_cfg;
}

void config_get_copy(app_config_t *dst)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(dst, &s_cfg, sizeof(*dst));
    xSemaphoreGive(s_cfg_mutex);
}

esp_err_t config_store_commit(const app_config_t *src, cfg_result_t *reason)
{
    app_config_t staged;
    memcpy(&staged, src, sizeof(staged));
    app_config_seal(&staged);

    uint8_t which = 0xFF;
    cfg_result_t vr = app_config_validate(&staged, &which);
    if (vr != CFG_OK) {
        if (reason) *reason = vr;
        ESP_LOGW(TAG, "rejected config: %s (spindle %u)",
                 app_config_result_str(vr), which);
        return ESP_ERR_INVALID_ARG;
    }
    if (reason) *reason = CFG_OK;

    esp_err_t err = write_slot(KEY_ACTIVE, &staged);
    if (err != ESP_OK) return err;

    /* Swap in one memcpy under the mutex. Consumers re-read config_get()
     * on each evaluation pass, so they pick up the change on the next
     * cycle without any restart (UI-R7). */
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(&s_cfg, &staged, sizeof(s_cfg));
    xSemaphoreGive(s_cfg_mutex);

    ESP_LOGI(TAG, "configuration updated");
    return ESP_OK;
}

esp_err_t config_store_mark_good(void)
{
    app_config_t snapshot;
    config_get_copy(&snapshot);
    return write_slot(KEY_KNOWN_GOOD, &snapshot);
}

esp_err_t config_store_factory_reset(void)
{
    app_config_t defaults;
    app_config_set_defaults(&defaults);

    esp_err_t err = write_slot(KEY_ACTIVE, &defaults);
    if (err == ESP_OK) err = write_slot(KEY_KNOWN_GOOD, &defaults);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(&s_cfg, &defaults, sizeof(s_cfg));
    xSemaphoreGive(s_cfg_mutex);

    ESP_LOGW(TAG, "factory reset applied");
    return ESP_OK;
}
