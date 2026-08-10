/*
 * app_config.h — the complete user-configurable state of the device.
 *
 * Deliberately free of ESP-IDF includes: this header and app_config.c are
 * pure C so the validation logic can be compiled and tested on a host
 * machine (see host_test/). Persistence lives in config_store.[ch].
 *
 * Everything here is serialised as one versioned, CRC-checked blob. That
 * makes export/import (UI-R8) and known-good rollback (UI-R9) trivial:
 * both are just "write the blob" and "keep the previous blob".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on any layout change. config_store refuses to load a blob whose
 * version it does not understand, and falls back to defaults. */
#define CONFIG_SCHEMA_VERSION   2

#define CFG_NAME_LEN            24
#define CFG_UNIT_LEN            8
#define CFG_WIFI_SSID_LEN       32
#define CFG_WIFI_PASS_LEN       64

/* ============================================================
 * Enumerations
 * ============================================================ */

typedef enum {
    PRESSURE_INPUT_4_20MA = 0,
    PRESSURE_INPUT_0_10V  = 1,
} pressure_input_mode_t;

/* Which quantity a threshold set applies to. Ordering is used to index
 * arrays, so do not reorder without updating CONFIG_SCHEMA_VERSION. */
typedef enum {
    QTY_CURRENT  = 0,
    QTY_PRESSURE = 1,
    QTY_RPM      = 2,
    QTY_COUNT
} quantity_t;

/* Four-band limits (SRS §6.1). Order is low-to-high so that a simple
 * comparison can validate LoLo <= Lo <= Hi <= HiHi. */
typedef enum {
    BAND_LOLO = 0,
    BAND_LO   = 1,
    BAND_HI   = 2,
    BAND_HIHI = 3,
    BAND_COUNT
} band_t;

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

/* What drives a digital output (SRS DO-R1). */
typedef enum {
    DO_SRC_DISABLED = 0,
    DO_SRC_SPINDLE_ALARM,    /* critical band, breakage or crash, one spindle */
    DO_SRC_SPINDLE_WARNING,  /* warning band or wear trend, one spindle       */
    DO_SRC_ANY_ALARM,        /* logical OR across both spindles               */
    DO_SRC_ANY_WARNING,
    DO_SRC_SYSTEM_HEALTHY,   /* de-asserts on hang or fault — fail-safe       */
    DO_SRC_DIAG_FAULT,       /* sensor/hardware faults, distinct from process */
    DO_SRC_COUNT
} do_source_t;

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
    float zero_offset_v;     /* measured bias, refreshed by auto-zero (AI-R7) */

    /* Number of ADC samples in one RMS burst. See analog.c for the accuracy
     * trade-off this controls — it is the single most consequential number
     * in the current measurement chain given the ADS1115's 860 SPS ceiling. */
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
    uint16_t pulses_per_rev;      /* 1..1024 (AI-R9)                          */
    uint16_t glitch_filter_ns;    /* hardware filter; see rpm.c for the cap   */
    uint16_t zero_timeout_ms;     /* no pulses for this long => RPM 0 (AI-R12)*/
} rpm_cfg_t;

/* ============================================================
 * Spindle state machine thresholds (SRS §5.1)
 * ============================================================ */

typedef struct {
    float    start_rpm;           /* above this, spindle is turning           */
    uint16_t settle_ms;           /* RPM stable this long => leave SPIN_UP    */
    float    idle_current_a;      /* below this while turning => IDLE         */
    float    cut_detect_current_a;/* above this => CUTTING, monitoring active */
    uint16_t alarm_inhibit_ms;    /* suppress alarms after entering CUTTING   */
} spindle_sm_cfg_t;

/* ============================================================
 * Tool wear / breakage detection (SRS §6.2)
 *
 * Only the detector parameters live here. Learned tool profiles are bulk
 * data and get their own store — keeping them out of this blob stops a
 * 32-profile array from bloating every config read and write.
 * ============================================================ */

typedef struct {
    bool     breakage_enabled;
    uint8_t  breakage_drop_pct;   /* sudden fall, % of recent mean (TW-R8)    */
    uint16_t breakage_window_ms;

    bool     crash_enabled;
    uint8_t  crash_rise_pct;      /* sudden rise (TW-R9)                      */
    uint16_t crash_window_ms;

    bool     trend_enabled;
    uint8_t  trend_cycles;        /* consecutive rising cycles => alarm       */

    float    adaptive_k_warn;     /* baseline_mean + k*sigma, warning band    */
    float    adaptive_k_alarm;
} wear_cfg_t;

/* ============================================================
 * Per-spindle aggregate
 * ============================================================ */

typedef struct {
    bool             enabled;
    char             name[CFG_NAME_LEN];

    current_cfg_t    current;
    pressure_cfg_t   pressure;
    rpm_cfg_t        rpm;
    spindle_sm_cfg_t sm;
    wear_cfg_t       wear;

    /* [quantity][band] */
    band_cfg_t       bands[QTY_COUNT][BAND_COUNT];
} spindle_cfg_t;

/* ============================================================
 * Digital output mapping
 * ============================================================ */

typedef struct {
    do_source_t source;
    uint8_t     spindle;        /* which spindle, for the per-spindle sources */
    bool        invert;         /* true => normally-closed / fail-safe        */
    uint16_t    min_pulse_ms;   /* guarantee the PLC scan catches it (DO-R4)  */
} do_cfg_t;

/* ============================================================
 * Network / Modbus (UI-R… web configuration of comms parameters)
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
    uint16_t     measure_period_ms;   /* acquisition cycle, default 100 (10 Hz) */
    uint16_t     log_interval_s;      /* 1..3600 (LG-R1)                        */
    uint8_t      mains_hz;            /* 50 or 60 — sets the RMS burst window   */

    wifi_cfg_t   wifi;
    modbus_cfg_t modbus;
} system_cfg_t;

/* ============================================================
 * Root
 * ============================================================ */

typedef struct {
    uint16_t      schema_version;
    uint16_t      _pad;
    system_cfg_t  system;
    spindle_cfg_t spindle[NUM_SPINDLES];
    do_cfg_t      dout[NUM_DIGITAL_OUT];
    uint32_t      crc32;          /* over everything above; must stay last    */
} app_config_t;

/* ============================================================
 * API — all pure, no I/O
 * ============================================================ */

/* Populate cfg with the factory defaults. Always succeeds. */
void app_config_set_defaults(app_config_t *cfg);

/* Validation result. Reports the first problem found rather than a list,
 * because the UI validates field-by-field anyway (UI-R6) and this is the
 * server-side backstop. */
typedef enum {
    CFG_OK = 0,
    CFG_ERR_SCHEMA,
    CFG_ERR_BAND_ORDER,       /* LoLo > Lo, or Hi > HiHi, etc.           */
    CFG_ERR_SENSOR_SPAN,      /* sensor_min >= sensor_max                */
    CFG_ERR_PPR_RANGE,
    CFG_ERR_CT_RANGE,
    CFG_ERR_SM_THRESHOLD,     /* idle >= cut detect                      */
    CFG_ERR_PERIOD_RANGE,
    CFG_ERR_DO_SOURCE,
    CFG_ERR_BURST_RANGE,
    CFG_ERR_MAINS_HZ,
    CFG_ERR_MODBUS_RANGE,
} cfg_result_t;

/* Check every constraint that could make the device behave unsafely.
 * On failure, *which_spindle is set when the fault is spindle-specific,
 * or 0xFF for a global fault. Pass NULL if you do not care. */
cfg_result_t app_config_validate(const app_config_t *cfg, uint8_t *which_spindle);

const char *app_config_result_str(cfg_result_t r);

/* CRC over the struct excluding the trailing crc32 field. */
uint32_t app_config_crc(const app_config_t *cfg);

/* Compute and store the CRC. Call before persisting. */
void app_config_seal(app_config_t *cfg);

/* Verify version and CRC of a loaded blob. */
bool app_config_check(const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
