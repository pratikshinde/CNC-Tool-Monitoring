/*
 * app_config.c — ESP32 root config: defaults, validation, CRC/seal/check.
 *
 * Pure C, no ESP-IDF (still — only board.h's #defines are used, no driver
 * headers), but no longer compiled into the SMU: this is the root
 * aggregate around two spindle_cfg_t instances plus WiFi/Modbus, which is
 * meaningless off the ESP32. The per-spindle logic this file delegates to
 * (spindle_cfg_set_defaults / spindle_cfg_validate) lives in
 * components/smu_shared/spindle_config.c and is what the SMU actually
 * shares. See app_config.h for the full rationale.
 */

#include "app_config.h"

#include <stddef.h>
#include <string.h>

/* ============================================================
 * CRC-32 (IEEE 802.3, reflected) — small table-free implementation.
 * Config blobs are a couple of hundred bytes, so the bitwise loop costs
 * nothing and saves a 1 KB table. This CRC guards the ESP32's own NVS
 * blob (config_store.c) and is unrelated to the CRC-16 that guards the
 * I2C wire protocol (smu_crc16(), in smu_shared) — different data,
 * different medium, no reason to share one algorithm between them.
 * ============================================================ */

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

uint32_t app_config_crc(const app_config_t *cfg)
{
    /* Everything except the trailing crc32 field. */
    const size_t span = offsetof(app_config_t, crc32);
    return crc32_update(0, (const uint8_t *)cfg, span);
}

void app_config_seal(app_config_t *cfg)
{
    cfg->schema_version = CONFIG_SCHEMA_VERSION;
    cfg->crc32 = app_config_crc(cfg);
}

bool app_config_check(const app_config_t *cfg)
{
    if (cfg->schema_version != CONFIG_SCHEMA_VERSION) return false;
    return cfg->crc32 == app_config_crc(cfg);
}

/* ============================================================
 * Defaults
 * ============================================================ */

void app_config_set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->schema_version = CONFIG_SCHEMA_VERSION;

    cfg->system.measure_period_ms = 100;   /* legacy field, see app_config.h */
    cfg->system.mains_hz          = 50;

    /* WiFi STA left blank: the device relies on its always-on fallback AP
     * (wifi.c) for initial reachability until an operator sets these. */
    cfg->system.wifi.ssid[0]     = '\0';
    cfg->system.wifi.password[0] = '\0';

    /* Modbus defaults match the old Arduino sketch's Settings defaults. */
    cfg->system.modbus.rtu_enabled = true;
    cfg->system.modbus.baud        = 19200;
    cfg->system.modbus.parity      = CFG_PARITY_EVEN;
    cfg->system.modbus.stop_bits   = 1;
    cfg->system.modbus.data_bits   = 8;
    cfg->system.modbus.slave_id    = 1;
    cfg->system.modbus.tcp_enabled = true;
    cfg->system.modbus.tcp_port    = 502;

    for (int i = 0; i < NUM_SPINDLES; i++) {
        spindle_cfg_set_defaults(&cfg->spindle[i], i);
    }

    app_config_seal(cfg);
}

/* ============================================================
 * Validation — root-level checks, then delegate per spindle
 * ============================================================ */

cfg_result_t app_config_validate(const app_config_t *cfg, uint8_t *which)
{
    if (which) *which = 0xFF;

    if (cfg->schema_version != CONFIG_SCHEMA_VERSION) return CFG_ERR_SCHEMA;

    if (cfg->system.measure_period_ms < 20 || cfg->system.measure_period_ms > 1000) {
        return CFG_ERR_PERIOD_RANGE;
    }
    if (cfg->system.mains_hz != 50 && cfg->system.mains_hz != 60) {
        return CFG_ERR_MAINS_HZ;
    }

    {
        const modbus_cfg_t *m = &cfg->system.modbus;
        if (m->baud < 300 || m->baud > 115200)        return CFG_ERR_MODBUS_RANGE;
        if (m->stop_bits != 1 && m->stop_bits != 2)   return CFG_ERR_MODBUS_RANGE;
        if (m->slave_id < 1 || m->slave_id > 247)     return CFG_ERR_MODBUS_RANGE;
        if (m->parity != CFG_PARITY_NONE && m->parity != CFG_PARITY_ODD &&
            m->parity != CFG_PARITY_EVEN) {
            return CFG_ERR_MODBUS_RANGE;
        }
        /* RTU framing is fixed at 8 data bits by the protocol — 7 only
         * exists for ASCII mode, which this device does not implement.
         * Reject it here rather than silently reinterpreting it. */
        if (m->rtu_enabled && m->data_bits != 8)      return CFG_ERR_MODBUS_RANGE;
        if (m->data_bits != 7 && m->data_bits != 8)   return CFG_ERR_MODBUS_RANGE;
        if (m->tcp_enabled && m->tcp_port == 0)       return CFG_ERR_MODBUS_RANGE;
    }

    for (uint8_t i = 0; i < NUM_SPINDLES; i++) {
        const spindle_cfg_t *s = &cfg->spindle[i];
        if (!s->enabled) continue;
        if (which) *which = i;

        cfg_result_t r = spindle_cfg_validate(s);
        if (r != CFG_OK) return r;
    }

    if (which) *which = 0xFF;
    return CFG_OK;
}
