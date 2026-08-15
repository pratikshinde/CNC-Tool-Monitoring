/*
 * web.h — settings and live-telemetry web UI.
 *
 * One embedded page (main/web/index.html, linked in via EMBED_TXTFILES)
 * with a WiFi/Modbus settings form and a 1 Hz-polled telemetry panel. Not
 * the Phase 4 SPA — that is a separate, later effort.
 */
#pragma once

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the HTTP server and registers all routes. Assumes wifi_init() has
 * already brought up networking. */
esp_err_t web_init(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
