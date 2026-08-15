/*
 * spindle_config.h — the per-spindle configuration schema.
 *
 * Portable, no ESP-IDF, no board.h. This is deliberate and load-bearing:
 * everything in this file is compiled BOTH into the ESP32 master image and
 * directly into the SMU (arm-none-eabi) image. It describes the shape of
 * one spindle's calibration and thresholds — the same shape the shared
 * wire protocol (smu_proto.h's smu_config_t) carries between them.
 *
 * What is NOT here, and why: WiFi, Modbus, and the digital-output mapping
 * are ESP32-only concerns (network/UI authority, and in V2 the ESP32 no
 * longer drives any physical output — see FIRMWARE_DESIGN_SPEC.md §2.3).
 * Those live in main/app_config.h, the ESP32-only root aggregate, which
 * includes this header for spindle_cfg_t and wraps it in
 * `spindle_cfg_t spindle[NUM_SPINDLES]`.
 *
 * Renamed from app_config.h (V1) specifically to avoid a same-basename
 * collision on the include path against main/app_config.h — see
 * FIRMWARE_DESIGN_SPEC.md §4.3.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_NAME_LEN            24
#define CFG_UNIT_LEN            8

/* ============================================================
 * Enumerations
 * ============================================================ */

typedef enum {
    PRESSURE_INPUT_4_20MA = 0,
    PRESSURE_INPUT_0_10V  = 1,
} pressure_input_mode_t;

/* Which quantity a threshold set applies to. Ordering is used to index
 * arrays, so do not reorder without a schema version bump on the side
 * that owns the array (root app_config_t on ESP32, smu_config_t on the
 * wire — see smu_proto.h's SMU_QTY_* mirror of this enum). */
typedef enum {
    QTY_CURRENT  = 0,
    QTY_PRESSURE = 1,
    QTY_RPM      = 2,
    QTY_COUNT
} quantity_t;

/* Four-band limits. Order is low-to-high so a simple comparison can
 * validate LoLo <= Lo <= Hi <= HiHi. */
typedef enum {
    BAND_LOLO = 0,
    BAND_LO   = 1,
    BAND_HI   = 2,
    BAND_HIHI = 3,
    BAND_COUNT
} band_t;

/* ============================================================
 * Per-band threshold configuration
 * ============================================================ */

typedef struct {
    bool     enabled;
    float    limit;          /* engineering units                            */
    float    hysteresis;     /* engineering units, applied when clearing     */
    uint16_t on_delay_ms;    /* must persist this long before asserting      */
    uint16_t off_delay_ms;   /* must be clear this long before de-asserting  */
    bool     latching;       /* requires explicit acknowledgement to clear   */
} band_cfg_t;

/* ============================================================
 * Per-channel configuration
 * ============================================================ */

typedef struct {
    float ct_primary_amps;   /* CT rating, e.g. 50 A                          */
    float ct_secondary_ma;   /* CT output at rated primary, e.g. 50 mA        */
    float gain_correction;   /* field calibration multiplier, nominal 1.0     */
    float zero_offset_v;     /* measured bias, refreshed by auto-zero         */

    /* Readings below this report exactly zero.
     *
     * An RMS is the square root of a sum of squares, so it is always
     * positive — with no load, what comes out is the RMS of the noise
     * floor, not zero. Without a deadband an idle spindle reads a small
     * non-zero current forever, which looks like a fault to an operator
     * and defeats any LoLo band set near zero. */
    float noload_cutoff_a;

    /* Number of ADC samples in one RMS burst — the single most
     * consequential number in the current measurement chain. */
    uint16_t rms_burst_samples;
} current_cfg_t;

typedef struct {
    pressure_input_mode_t mode;
    float sensor_min;        /* engineering value at 4 mA / 0 V   */
    float sensor_max;        /* engineering value at 20 mA / 10 V */
    char  unit[CFG_UNIT_LEN];/* "bar", "psi", "kPa", "MPa"        */
    float gain_correction;
    float offset_correction; /* engineering units */
} pressure_cfg_t;

typedef struct {
    uint16_t pulses_per_rev;      /* 1..1024                                  */
    uint16_t glitch_filter_ns;    /* hardware filter cap                      */
    uint16_t zero_timeout_ms;     /* no pulses for this long => RPM 0         */
} rpm_cfg_t;

/* ============================================================
 * Spindle state machine thresholds
 * ============================================================ */

typedef struct {
    float    start_rpm;           /* above this, spindle is turning           */
    uint16_t settle_ms;           /* RPM stable this long => leave SPIN_UP    */
    float    idle_current_a;      /* below this while turning => IDLE         */
    float    cut_detect_current_a;/* above this => CUTTING, monitoring active */
    uint16_t alarm_inhibit_ms;    /* suppress alarms after entering CUTTING   */
} spindle_sm_cfg_t;

/* ============================================================
 * Tool wear / breakage detection
 *
 * Only the detector parameters live here. Learned tool profiles are bulk
 * data and get their own store.
 * ============================================================ */

typedef struct {
    bool     breakage_enabled;
    uint8_t  breakage_drop_pct;   /* sudden fall, % of recent mean            */
    uint16_t breakage_window_ms;

    bool     crash_enabled;
    uint8_t  crash_rise_pct;      /* sudden rise                              */
    uint16_t crash_window_ms;

    bool     trend_enabled;
    uint8_t  trend_cycles;        /* consecutive persisting cycles => raise   */

    /* Adaptive wear baseline (FIRMWARE_DESIGN_SPEC.md §3.8). The baseline
     * is learned once over baseline_learn_cycles admitted cycles and then
     * FROZEN — a continuously-adapting baseline would track a blunting
     * tool upward and detect nothing. */
    uint8_t  baseline_learn_cycles;      /* 5..64, default 20                 */
    uint8_t  baseline_sigma_floor_pct;   /* 1..20, default 2                  */
    uint8_t  baseline_sigma_ceiling_pct; /* 5..100, default 25 — above this
                                          * the process is too variable and
                                          * the baseline is rejected          */

    float    adaptive_k_warn;     /* median + k*sigma, trend warning          */
    float    adaptive_k_alarm;    /* must exceed adaptive_k_warn              */
} wear_cfg_t;

/* ============================================================
 * Per-spindle aggregate — the shape carried over the wire
 * ============================================================ */

typedef struct {
    bool             enabled;
    char             name[CFG_NAME_LEN];

    /* Whether the Machine Running level input (SMU pin 19,
     * FIRMWARE_DESIGN_SPEC.md §3.3) participates in arming. When false,
     * arming falls back to local CUTTING-detection alone — V1's
     * behaviour. Fault Clear (pin 20) has no such toggle; it is always
     * active. */
    bool             machine_running_enabled;

    /* Minimum time an SMU output stays asserted once it fires, so a slow
     * PLC scan cannot step over a brief event (DO-R4). V1 held this per
     * output-channel on the ESP32's do_cfg_t; V2's SMU outputs are a
     * fixed mapping (FIRMWARE_DESIGN_SPEC.md §3.4), so one value per
     * spindle governs all four of that SMU's outputs. */
    uint16_t         min_pulse_ms;

    current_cfg_t    current;
    pressure_cfg_t   pressure;
    rpm_cfg_t        rpm;
    spindle_sm_cfg_t sm;
    wear_cfg_t       wear;

    /* [quantity][band] */
    band_cfg_t       bands[QTY_COUNT][BAND_COUNT];
} spindle_cfg_t;

/* ============================================================
 * Validation result
 *
 * One shared enum for both the per-spindle validator below and the
 * ESP32-only root validator (main/app_config.h) — a result code returned
 * by either is meaningful without the caller needing to know which one
 * produced it.
 * ============================================================ */

typedef enum {
    CFG_OK = 0,
    CFG_ERR_SCHEMA,
    CFG_ERR_BAND_ORDER,       /* LoLo > Lo, or Hi > HiHi, etc.           */
    CFG_ERR_SENSOR_SPAN,      /* sensor_min >= sensor_max                */
    CFG_ERR_PPR_RANGE,
    CFG_ERR_CT_RANGE,
    CFG_ERR_SM_THRESHOLD,     /* idle >= cut detect                      */
    CFG_ERR_PERIOD_RANGE,
    CFG_ERR_BURST_RANGE,
    CFG_ERR_MAINS_HZ,
    CFG_ERR_MODBUS_RANGE,
    CFG_ERR_WEAR_BASELINE,    /* adaptive_k ordering or sigma pct ordering */
} cfg_result_t;

/* ============================================================
 * API — all pure, no I/O
 * ============================================================ */

/* Populate one spindle with factory defaults. `index` (0-based) is used
 * only to derive the default name ("Spindle 1", "Spindle 2", ...).
 * Shared so the ESP32's root defaults and an SMU's own compiled-in
 * fallback (used if its Data Flash calibration is unreadable — see
 * FIRMWARE_DESIGN_SPEC.md §3.5) are the same defaults, not two that can
 * drift apart. */
void spindle_cfg_set_defaults(spindle_cfg_t *s, int index);

/* Validate one spindle's configuration in isolation — everything an SMU
 * can check about the block it receives over the wire, and everything
 * the ESP32 checks per-spindle before folding in its own root-level
 * checks (period, mains Hz, Modbus, output mapping). */
cfg_result_t spindle_cfg_validate(const spindle_cfg_t *s);

const char *app_config_result_str(cfg_result_t r);

#ifdef __cplusplus
}
#endif
