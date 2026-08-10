/*
 * config_store.h — power-fail-safe persistence for app_config_t (UI-R9).
 *
 * Two slots are kept in NVS: "active" and "known_good". A save writes the
 * active slot first; only once the device has run for a while without
 * incident is that copy promoted to known_good. If the active slot fails
 * its CRC on boot, known_good is used instead, and if that is also bad the
 * factory defaults are applied. Power can be lost at any point in this
 * sequence without leaving the device unconfigured.
 */
#pragma once

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFG_LOADED_ACTIVE,      /* normal path                                   */
    CFG_LOADED_KNOWN_GOOD,  /* active slot was corrupt — recovered           */
    CFG_LOADED_DEFAULTS,    /* both slots unusable, or first ever boot       */
} config_load_source_t;

/* Mount NVS and load configuration into the global instance.
 * Never fails in a way that leaves the device without config. */
esp_err_t config_store_init(config_load_source_t *source_out);

/* The live configuration. Read freely from any task; treat as immutable.
 * Writers must go through config_store_commit(). */
const app_config_t *config_get(void);

/* Take a working copy to edit. */
void config_get_copy(app_config_t *dst);

/* Validate, seal and persist. The live config is swapped atomically under
 * a mutex, so a reader never observes a half-updated structure and the
 * change takes effect without a reboot (UI-R7).
 * Returns ESP_ERR_INVALID_ARG with *reason set if validation fails. */
esp_err_t config_store_commit(const app_config_t *src, cfg_result_t *reason);

/* Promote the active slot to known-good. Called once the application has
 * been running cleanly for a while. */
esp_err_t config_store_mark_good(void);

/* Restore factory defaults and persist them. */
esp_err_t config_store_factory_reset(void);

#ifdef __cplusplus
}
#endif
