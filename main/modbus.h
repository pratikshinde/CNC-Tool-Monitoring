/*
 * modbus.h — Modbus RTU (RS485) and Modbus TCP slaves.
 *
 * Read-only holding registers publishing the same picture the alarm engine
 * has: per-spindle current/pressure/RPM plus state and severity, and
 * system health. This is a monitoring device, not a control point, so
 * nothing here is writable from the bus (MB_ACCESS_RO) — see modbus.c for
 * the register map layout.
 */
#pragma once

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts whichever of RTU/TCP are enabled in cfg->system.modbus, and the
 * background task that refreshes the register map from monitor_get_snapshot()
 * (monitor.h). Safe to call with both interfaces disabled — the refresh task
 * still runs, idle, so a later reconfigure can enable one without a reboot. */
esp_err_t modbus_init(const app_config_t *cfg);

/* Tear down and recreate the RTU/TCP slave instances with new settings
 * (baud, parity, slave ID, TCP port, enabled flags). The register refresh
 * task is untouched. */
esp_err_t modbus_reconfigure(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
