/*
 * app_config.h — the complete user-configurable state of the ESP32 master.
 *
 * This is the ESP32-ONLY root aggregate: WiFi and Modbus, wrapped around
 * two `spindle_cfg_t` instances. It is deliberately NOT portable — it
 * depends on `board.h` for NUM_SPINDLES and has no reason to compile on
 * the SMU, which never sees more than its own single spindle_cfg_t
 * (carried over the wire as smu_config_t instead of this whole struct —
 * see shared/smu_proto.h).
 *
 * V1 also carried a configurable digital-output-mapping schema here
 * (do_cfg_t, do_source_t, DO_SRC_* constants, dout[]). V2 retires it
 * entirely: the ESP32 drives no physical output at all, and each SMU's
 * four outputs are a fixed mapping in SMU firmware, not something this
 * config describes (FIRMWARE_DESIGN_SPEC.md §3.4, §2.3).
 *
 * The truly portable per-spindle schema (spindle_cfg_t and everything it
 * contains) lives in components/smu_shared/spindle_config.h, included
 * below. This file was split out of a single app_config.h during the V2
 * migration specifically to resolve that portability boundary — see
 * FIRMWARE_DESIGN_SPEC.md §4.3. The split kept every symbol name a
 * consumer (`config_store.c`, `monitor.c`, `web.c`, `modbus.c`, `calib.c`)
 * already used unchanged: `app_config_t`, `config_get()`,
 * `app_config_set_defaults()`, `app_config_validate()`, etc. all still mean
 * what they meant before. Only the file that defines them moved.
 *
 * Everything here is serialised as one versioned, CRC-checked blob. That
 * makes export/import and known-good rollback trivial: both are just
 * "write the blob" and "keep the previous blob" — see config_store.[ch].
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "spindle_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on any layout change. config_store refuses to load a blob whose
 * version it does not understand, and falls back to defaults.
 *
 * 7 -> 8: added wear_cfg_t.baseline_learn_cycles / baseline_sigma_floor_pct
 * / baseline_sigma_ceiling_pct (FIRMWARE_DESIGN_SPEC.md §3.8); split this
 * header from the portable spindle_config.h; and removed do_cfg_t /
 * do_source_t / DO_SRC_* / dout[] — V2 retires the ESP32's configurable
 * digital-output mapping entirely (do_map.c, deleted alongside dio.c),
 * since the ESP32 drives no physical output in V2 at all. Each SMU's
 * four outputs are a fixed mapping in SMU firmware instead
 * (FIRMWARE_DESIGN_SPEC.md §3.4, §2.3). */
#define CONFIG_SCHEMA_VERSION   8

#define CFG_WIFI_SSID_LEN       32
#define CFG_WIFI_PASS_LEN       64

/* ============================================================
 * ESP32-only enumerations
 * ============================================================ */

/* Modbus serial parity. RTU framing is fixed at 8 data bits by the
 * protocol itself (7 data bits only exists for ASCII framing, which is
 * not implemented) — see the validation note on modbus_cfg_t.data_bits.
 *
 * Named CFG_PARITY_* rather than MB_PARITY_* deliberately: the esp-modbus
 * library #defines MB_PARITY_NONE itself (mbcontroller.h), and colliding
 * with that would silently macro-substitute inside this enum wherever
 * both headers are visible. */
typedef enum {
    CFG_PARITY_NONE = 0,
    CFG_PARITY_ODD  = 1,
    CFG_PARITY_EVEN = 2,
} cfg_parity_t;

/* ============================================================
 * Network / Modbus
 * ============================================================ */

/* STA credentials. The device also always runs a fallback AP (see
 * wifi.c) so it can never become unreachable — that AP's own SSID/
 * password are device-generated, not stored here. */
typedef struct {
    char ssid[CFG_WIFI_SSID_LEN];
    char password[CFG_WIFI_PASS_LEN];
} wifi_cfg_t;

typedef struct {
    bool        rtu_enabled;
    uint32_t    baud;         /* 300..115200                                 */
    cfg_parity_t parity;
    uint8_t     stop_bits;    /* 1 or 2                                      */
    uint8_t     data_bits;    /* must be 8 while rtu_enabled (see above)     */
    uint8_t     slave_id;     /* 1..247                                      */

    bool        tcp_enabled;
    uint16_t    tcp_port;     /* default 502                                 */
} modbus_cfg_t;

/* ============================================================
 * System-wide
 * ============================================================ */

typedef struct {
    uint16_t     measure_period_ms;   /* legacy field — see note below        */
    uint8_t      mains_hz;            /* 50 or 60                             */

    wifi_cfg_t   wifi;
    modbus_cfg_t modbus;
} system_cfg_t;
/* measure_period_ms governed V1's local acquisition loop and has no
 * meaning once the ESP32 no longer samples anything itself (V2's SMU
 * loop period is a firmware constant on the SMU side, not configured
 * from here — see FIRMWARE_DESIGN_SPEC.md §3.1). Kept for schema
 * continuity through the migration; app_config_validate() still range
 * -checks it as a placeholder. Revisit when Phase 3/4 wires smu_link's
 * own polling cadence, at which point this field is a candidate for
 * removal in a future schema bump. */

/* ============================================================
 * Root
 * ============================================================ */

typedef struct {
    uint16_t      schema_version;
    uint16_t      _pad;
    system_cfg_t  system;
    spindle_cfg_t spindle[NUM_SPINDLES];
    uint32_t      crc32;          /* over everything above; must stay last    */
} app_config_t;

/* ============================================================
 * API — all pure, no I/O
 * ============================================================ */

/* Populate cfg with the factory defaults. Always succeeds. */
void app_config_set_defaults(app_config_t *cfg);

/* Check every constraint that could make the device behave unsafely.
 * On failure, *which_spindle is set when the fault is spindle-specific,
 * or 0xFF for a global fault. Pass NULL if you do not care.
 *
 * cfg_result_t is declared in spindle_config.h — shared with the
 * per-spindle validator this function calls internally. */
cfg_result_t app_config_validate(const app_config_t *cfg, uint8_t *which_spindle);

/* CRC over the struct excluding the trailing crc32 field. */
uint32_t app_config_crc(const app_config_t *cfg);

/* Compute and store the CRC. Call before persisting. */
void app_config_seal(app_config_t *cfg);

/* Verify version and CRC of a loaded blob. */
bool app_config_check(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
