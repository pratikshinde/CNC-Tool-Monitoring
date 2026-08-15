/*
 * monitor.h — the published telemetry snapshot and its access API.
 *
 * V1: owned the real-time acquisition/alarm task on core 0. V2: the
 * struct shape and API below are the stable contract every consumer
 * (web.c, modbus.c, calib.c, trend.c, main.c) builds against; what
 * populates them changed from local acquisition (V1) to an SMU I2C link
 * aggregator (V2 migration Phase 3) — see monitor.c.
 *
 * Phase 4 note: this snapshot carries everything smu_telemetry_t
 * (shared/smu_proto.h) actually reports today — band bitmaps, diagnostic
 * flags, transient detector state, link health. It does NOT yet carry
 * the adaptive-baseline state or the rolling-window statistics
 * MODBUS_REGISTER_MAP.md §7 describes: those aren't on the wire at all
 * yet (smu_telemetry_t has no baseline fields), so there is nothing here
 * to wire them FROM. Extending the wire protocol to add them is a
 * separate, deliberately out-of-scope change from "expose what the SMU
 * already reports" — conflating the two would have made this expansion
 * open-ended instead of a bounded pass over existing data.
 *
 * analog.h and rpm.h are deliberately NOT included here any more: this
 * header no longer needs anything from either beyond sensor_status_t,
 * which lives in scaling.h (a smu_shared header, not an ESP32-only one)
 * and is included directly below instead.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alarm.h"
#include "app_config.h"
#include "scaling.h"
#include "smu_link.h"
#include "spindle_sm.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Published snapshot of one spindle, for the UI and the logger.
 * Copied out under a mutex; never referenced directly by other tasks. */
typedef struct {
    spindle_state_t state;
    bool            monitoring_armed;

    float           current_a;
    float           current_avg_a;   /* 1 s moving average (AI-R6) */
    float           current_peak_a;  /* peak hold this cut          */
    float           pressure;
    float           rpm;

    /* Pre-scaling values, republished so the calibration screen can show
     * what it is actually working from and so calib.c can recompute an
     * uncorrected engineering value without going back to the ADC and
     * contending with the measurement loop for the bus. */
    float           burden_vrms;
    float           pressure_adc_volts;
    float           pressure_loop_ma;

    sensor_status_t current_status;
    sensor_status_t pressure_status;
    bool            rpm_sensor_suspect;  /* current flowing, no pulses */

    severity_t      severity;
    bool            any_alarm;
    bool            any_warning;
    bool            any_latched;
    bool            breakage;
    bool            crash;
    bool            trend;

    /* Per-quantity band state, index with QTY_CURRENT/QTY_PRESSURE/
     * QTY_RPM, test with (1u << BAND_LOLO)..(1u << BAND_HIHI) — mirrors
     * smu_telemetry_t.bands_active/bands_latched exactly (see
     * SMU_BAND_*_BIT in smu_proto.h), kept as bitmaps rather than
     * unpacked into 12 bools for the same reason the wire format does:
     * one field the UI/Modbus layer indexes into, not twelve to keep in
     * sync. */
    uint8_t         bands_active[QTY_COUNT];
    uint8_t         bands_latched[QTY_COUNT];

    /* Read-back of the SMU's actual hard-wired outputs (SMU_OUT_* bits) —
     * what the PLC is really seeing, not a recomputation of what it
     * should be seeing. Distinguishing those two is the entire point of
     * publishing this rather than deriving it from severity/bands here. */
    uint8_t         output_state;

    /* Raw diagnostic bitmask (SMU_DIAG_*) alongside the two fields V1
     * already had a dedicated place for (current_status/pressure_status/
     * rpm_sensor_suspect above). Kept as a raw mask rather than more
     * named bools because most of it — SMU_DIAG_RUN_NO_LOAD/LOAD_NO_RUN/
     * RUN_STUCK/PRESSURE_MODE/CAL_DEFAULTED/ADC_FAULT/SELFTEST_FAIL —
     * has no ESP32-side equivalent to unpack into yet; a consumer that
     * needs one of these bits by name should test diag_flags directly
     * against the SMU_DIAG_* constant, not wait for this struct to grow
     * a bool for every one of them individually. */
    uint16_t        diag_flags;

    uint32_t        cycle_count;
    uint32_t        last_cycle_ms;
    float           last_cycle_mean_a;
    float           last_cycle_peak_a;
} spindle_snapshot_t;

/* Per-SMU link health, for the "SMU status" panel
 * (FIRMWARE_DESIGN_SPEC.md §5.4) and for Modbus's link-supervision block
 * (MODBUS_REGISTER_MAP.md §3) — without this an operator cannot tell a
 * working system from a blind one, which the design docs call out
 * repeatedly as the one thing this UI must never let happen silently. */
typedef struct {
    smu_link_state_t state;
    bool             have_ever_linked;   /* distinguishes DOWN-never-seen from DOWN-lost */
    uint32_t         age_ms;             /* since the last good frame, 0 if never had one */
    uint32_t         crc_error_count;
    uint32_t         timeout_count;
    uint32_t         poll_count;

    uint16_t         fw_version;
    uint8_t          reset_cause;
    uint16_t         reset_count;
    uint32_t         smu_uptime_ms;

    /* Config schema version the SMU last reported applying (smu_ident_t.
     * cfg_schema_version), for the "SMU status" panel's config-sync
     * indicator (FIRMWARE_DESIGN_SPEC.md §5.4, plan Phase 5 bullet 3).
     * Compare against CONFIG_SCHEMA_VERSION (app_config.h) — the version
     * this build always pushes — to tell "config confirmed applied" from
     * "a push is still in flight or was never acknowledged". Zero if no
     * ident has ever been read (ident_valid false in monitor.c). */
    uint16_t         cfg_schema_version;
} smu_link_status_t;

typedef struct {
    spindle_snapshot_t spindle[NUM_SPINDLES];
    smu_link_status_t  link[NUM_SPINDLES];

    bool               adc_healthy;      /* V2: "are both SMU links fresh" — see monitor.c */
    bool               system_healthy;
    bool               diagnostic_fault;
    uint32_t           loop_period_ms;   /* measured, not configured */
    uint32_t           worst_loop_ms;
    int64_t            uptime_us;
} monitor_snapshot_t;

esp_err_t monitor_start(void);

/* Thread-safe copy of the latest published state. */
void monitor_get_snapshot(monitor_snapshot_t *out);

/* Acknowledge latched alarms on one spindle, or all if spindle >= count. */
void monitor_acknowledge(uint8_t spindle);

/* Re-read configuration after a commit. Cheap and non-blocking: the task
 * picks up the new pointer contents on its next pass. */
void monitor_config_changed(void);

/* Issue a command to one spindle's SMU and block for its result — see
 * smu_link_send_command_wait(). monitor.c owns the smu_link_t instances
 * privately (they are not part of monitor_snapshot_t, which is telemetry,
 * not a control surface), so this is the one door a request/response
 * caller — calib.c's auto-zero handler, currently the only one — goes
 * through to reach them. Never call this from a hot path: it blocks for
 * up to max_wait_us on a caller's own task, which is fine from an HTTP
 * handler and wrong from anything running at 20 Hz. */
esp_err_t monitor_smu_command_wait(uint8_t spindle, uint8_t opcode,
                                   uint8_t arg8, uint16_t arg16,
                                   int64_t max_wait_us, uint8_t *result_out);

#ifdef __cplusplus
}
#endif
