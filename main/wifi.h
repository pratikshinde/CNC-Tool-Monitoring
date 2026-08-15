/*
 * wifi.h — station connectivity with an always-on fallback access point.
 *
 * The device must never become unreachable from the web UI, even with
 * wrong or unset STA credentials — that is the same "never leave the
 * device unconfigurable" property config_store.h already guarantees for
 * the configuration blob itself. So the fallback AP is not a timed
 * failover: it is simply always running, alongside STA.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool    sta_connected;
    char    sta_ip[16];
    char    ap_ssid[33];
    char    ap_ip[16];
} wifi_status_t;

/* Bring up esp_netif/esp_event, start the fallback AP, and attempt an STA
 * join if cfg->system.wifi.ssid is non-empty. Never fails purely because
 * the STA join did not succeed — the AP is the guarantee of reachability. */
esp_err_t wifi_init(const app_config_t *cfg);

/* Re-apply STA credentials after a configuration change: disconnect,
 * reconfigure, and rejoin if a new SSID is set. Non-blocking. */
esp_err_t wifi_reconfigure(const app_config_t *cfg);

/* Thread-safe copy of the current connectivity state, for the web UI. */
void wifi_get_status(wifi_status_t *out);

#ifdef __cplusplus
}
#endif
