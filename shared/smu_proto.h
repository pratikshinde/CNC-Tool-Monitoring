/*
 * smu_proto.h — ESP32 <-> SMU I2C wire protocol.
 *
 * THIS FILE IS COMPILED INTO BOTH IMAGES. It is the single definition of
 * the register map, the frame layout, and every struct that crosses the
 * link. Do not fork it, do not maintain a second copy, and do not restate
 * these offsets in prose elsewhere — a master and a slave that disagree
 * about a struct layout corrupt calibration silently, which is the exact
 * failure this file exists to make impossible.
 *
 * Rules for changing anything here:
 *   1. Bump SMU_PROTO_VERSION on ANY layout change.
 *   2. Keep fields explicitly sized. No enums, no bitfields, no `bool`,
 *      no host-dependent types on the wire.
 *   3. Update the _Static_asserts at the bottom. They are the enforcement,
 *      and they are why this file is worth having.
 *
 * Endianness: little-endian on the wire. Both ends are little-endian
 * (Cortex-M23 and Xtensa LX6), so this costs nothing today; it is stated
 * so a future big-endian port knows what it owes.
 *
 * Persistence: the only thing an SMU stores across power loss is its
 * configuration block. Every counter and statistic in the telemetry frame
 * is since-power-on and volatile by design — see FIRMWARE_DESIGN_SPEC.md
 * §1.1 before adding a field that implies otherwise.
 *
 * Addressing: the register pointer is 16-BIT, big-endian, written as two
 * bytes before each read or write. An 8-bit pointer was the original
 * sketch and does not work — the band configuration array alone is 192
 * bytes, and the whole config block is 296, so the map does not fit in a
 * 256-byte space. Sixteen bits also leaves room to extend telemetry
 * without renumbering anything.
 *
 * See FIRMWARE_DESIGN_SPEC.md §5.2.
 */
#ifndef SMU_PROTO_H
#define SMU_PROTO_H

#include <stdint.h>

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#  error "smu_proto.h requires C11 for _Static_assert (build with -std=c11 or later)"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Versioning and identity
 * ============================================================ */

#define SMU_PROTO_MAGIC        0x534DU   /* "SM" */
#define SMU_PROTO_VERSION      1U

/* 7-bit I2C slave addresses. Deliberately different per spindle even
 * though each SMU sits on its own bus: a swapped daughter card or a
 * cross-wired bus then fails to ACK at bring-up, instead of silently
 * reporting one spindle's data as the other's. Both are clear of the
 * reserved 0x00-0x07 and 0x78-0x7F ranges. */
#define SMU_I2C_ADDR_SPINDLE_1 0x21U
#define SMU_I2C_ADDR_SPINDLE_2 0x22U

/* ============================================================
 * Register map — 16-bit byte offsets into the slave address space
 *
 * Windows are deliberately generous and aligned. Growth inside a window
 * costs nothing; growth that moves a base is a protocol version bump.
 * ============================================================ */

#define SMU_REG_IDENT          0x0000U   /* R   */
#define SMU_REG_TELEMETRY      0x0100U   /* R   */
#define SMU_REG_COMMAND        0x0200U   /* W   */
#define SMU_REG_CONFIG         0x0300U   /* R/W */
#define SMU_REG_WINDOW         0x0100U   /* per-window span, except config */
#define SMU_REG_CONFIG_WINDOW  0x0200U

/* ============================================================
 * Wire-format enumerations
 *
 * Plain #defines assigned to uint8_t fields rather than C enums: enum
 * size is implementation-defined and this header is shared between two
 * different compilers.
 * ============================================================ */

/* Spindle state — mirrors spindle_state_t */
#define SMU_STATE_STOPPED      0U
#define SMU_STATE_SPIN_UP      1U
#define SMU_STATE_IDLE         2U
#define SMU_STATE_CUTTING      3U
#define SMU_STATE_COAST_DOWN   4U

/* Severity — mirrors severity_t */
#define SMU_SEV_NONE           0U
#define SMU_SEV_DIAG           1U
#define SMU_SEV_TREND          2U
#define SMU_SEV_WARNING        3U
#define SMU_SEV_ALARM          4U
#define SMU_SEV_BREAKAGE       5U
#define SMU_SEV_CRASH          6U

/* Sensor status */
#define SMU_SENSOR_OK          0U
#define SMU_SENSOR_OPEN        1U
#define SMU_SENSOR_SHORT       2U
#define SMU_SENSOR_OVERRANGE   3U
#define SMU_SENSOR_UNCAL       4U

/* Reset cause */
#define SMU_RESET_POWER_ON     0U
#define SMU_RESET_WATCHDOG     1U
#define SMU_RESET_BROWNOUT     2U
#define SMU_RESET_SOFTWARE     3U
#define SMU_RESET_EXTERNAL     4U

/* Quantity and band indices — mirror quantity_t / band_t */
#define SMU_QTY_CURRENT        0U
#define SMU_QTY_PRESSURE       1U
#define SMU_QTY_RPM            2U
#define SMU_QTY_COUNT          3U
#define SMU_BAND_COUNT         4U

/* Bits within the per-quantity band bitmaps */
#define SMU_BAND_LOLO_BIT      (1U << 0)
#define SMU_BAND_LO_BIT        (1U << 1)
#define SMU_BAND_HI_BIT        (1U << 2)
#define SMU_BAND_HIHI_BIT      (1U << 3)

/* Diagnostic bits (smu_telemetry_t.diag_flags) */
#define SMU_DIAG_CURRENT_SENSOR   (1U << 0)
#define SMU_DIAG_PRESSURE_SENSOR  (1U << 1)
#define SMU_DIAG_RPM_SUSPECT      (1U << 2)  /* current but no pulses      */
#define SMU_DIAG_PRESSURE_MODE    (1U << 3)  /* jumper vs config mismatch  */
#define SMU_DIAG_RUN_NO_LOAD      (1U << 4)  /* running asserted, nothing seen */
#define SMU_DIAG_LOAD_NO_RUN      (1U << 5)  /* cutting current, run clear */
#define SMU_DIAG_RUN_STUCK        (1U << 6)
#define SMU_DIAG_CAL_DEFAULTED    (1U << 7)  /* running on safe defaults   */
#define SMU_DIAG_ADC_FAULT        (1U << 8)
#define SMU_DIAG_SELFTEST_FAIL    (1U << 9)

/* Status bits (smu_telemetry_t.status_flags) */
#define SMU_STATUS_ARMED          (1U << 0)
#define SMU_STATUS_MACHINE_RUN    (1U << 1)  /* live pin-19 level          */
#define SMU_STATUS_HEALTHY        (1U << 2)
#define SMU_STATUS_ANY_ALARM      (1U << 3)
#define SMU_STATUS_ANY_WARNING    (1U << 4)
#define SMU_STATUS_ANY_LATCHED    (1U << 5)
#define SMU_STATUS_BREAKAGE       (1U << 6)
#define SMU_STATUS_CRASH          (1U << 7)
#define SMU_STATUS_TREND          (1U << 8)
#define SMU_STATUS_CYCLE_CLOSED   (1U << 9)  /* cycle completed since last read */

/* Output state bits — read-back of the real hard-wired safety path,
 * not a recomputation of it. */
#define SMU_OUT_CURRENT_FAULT     (1U << 0)
#define SMU_OUT_PRESSURE_FAULT    (1U << 1)
#define SMU_OUT_RPM_FAULT         (1U << 2)
#define SMU_OUT_HEALTHY           (1U << 3)

/* Config flags (smu_config_t.flags) */
#define SMU_CFG_MACHINE_RUN_ENABLED (1U << 0)
#define SMU_CFG_SPINDLE_ENABLED     (1U << 1)

/* Commands (smu_command_t.opcode) */
#define SMU_CMD_NONE              0U
#define SMU_CMD_ACKNOWLEDGE       1U   /* clear latches no longer present  */
#define SMU_CMD_AUTOZERO          2U   /* refresh current zero-offset tare */
#define SMU_CMD_CONFIG_COMMIT     3U   /* validate + persist staged config */
#define SMU_CMD_CONFIG_ABORT      4U   /* discard staged config            */
#define SMU_CMD_RESET_TOOL_STATS  5U
#define SMU_CMD_RELEARN_BASELINE  6U
#define SMU_CMD_SOFT_RESET        7U

/* Command results (smu_ident_t.last_cmd_result) */
#define SMU_RESULT_OK             0U
#define SMU_RESULT_BUSY           1U
#define SMU_RESULT_BAD_OPCODE     2U
#define SMU_RESULT_BAD_CRC        3U
#define SMU_RESULT_CFG_INVALID    4U   /* app_config_validate() rejected   */
#define SMU_RESULT_CFG_VERSION    5U
#define SMU_RESULT_FLASH_FAIL     6U

/* ============================================================
 * Identity — SMU_REG_IDENT, read-only, 16 bytes
 * ============================================================ */

typedef struct __attribute__((packed)) {
    uint16_t magic;             /* SMU_PROTO_MAGIC                         */
    uint8_t  proto_version;     /* SMU_PROTO_VERSION                       */
    uint8_t  spindle_index;     /* 0 or 1, self-reported — cross-check it  */

    uint16_t fw_version;        /* (major << 8) | minor                    */
    uint8_t  reset_cause;       /* SMU_RESET_*                             */
    uint8_t  last_cmd_result;   /* SMU_RESULT_* for the most recent command */

    uint32_t uptime_ms;         /* since last SMU reset                    */

    /* Resets since power-on, NOT since manufacture. Deliberately not
     * persisted: the device keeps no historical data (see
     * FIRMWARE_DESIGN_SPEC.md §1.1). Still useful — it distinguishes a
     * single watchdog event from a board stuck in a reset loop, which is
     * the question this field is actually asked. */
    uint16_t reset_count;
    uint16_t cfg_schema_version;
} smu_ident_t;

/* ============================================================
 * Telemetry — SMU_REG_TELEMETRY, read-only, 72 bytes
 *
 * Double-buffered on the SMU so a read can never catch a half-updated
 * frame. `seq` increments on every publish: that is what lets the master
 * detect a stalled SMU that is still ACKing on the bus — a failure a CRC
 * alone would not catch, because stale bytes checksum perfectly.
 * ============================================================ */

typedef struct __attribute__((packed)) {
    uint16_t seq;               /* increments per published frame          */
    uint16_t status_flags;      /* SMU_STATUS_*                            */
    uint16_t diag_flags;        /* SMU_DIAG_*                              */
    uint8_t  state;             /* SMU_STATE_*                             */
    uint8_t  severity;          /* SMU_SEV_*                               */

    float    current_a;
    float    current_avg_a;     /* 1 s moving average                      */
    float    current_peak_a;    /* peak hold within the current cut        */
    float    pressure;
    float    rpm;

    /* Per-quantity band state. Index with SMU_QTY_*, test with
     * SMU_BAND_*_BIT. Bitmaps rather than a 12-element array: it saves
     * 18 bytes per frame and the master unpacks once. */
    uint8_t  bands_active[SMU_QTY_COUNT];
    uint8_t  bands_latched[SMU_QTY_COUNT];

    uint8_t  current_sensor;    /* SMU_SENSOR_*                            */
    uint8_t  pressure_sensor;   /* SMU_SENSOR_*                            */
    uint8_t  output_state;      /* SMU_OUT_* — actual pin state            */
    uint8_t  _pad0;

    /* Raw pre-scaling values. Cheap to carry, and the only way the master
     * can distinguish a drifting transmitter from a real process change. */
    float    ct_burden_vrms;
    float    pressure_adc_v;

    /* Closed-cycle summary. Valid when SMU_STATUS_CYCLE_CLOSED is set;
     * that flag self-clears once the frame has been read. */
    uint32_t cycle_count;
    uint32_t last_cycle_ms;
    float    last_cycle_mean_a;
    float    last_cycle_peak_a;

    uint32_t timestamp_ms;      /* SMU uptime at publish                   */
    uint16_t loop_worst_ms;     /* worst superloop period since reset      */
    uint16_t _reserved;         /* keeps the struct at 72 and 4-byte tidy  */
    uint16_t crc16;             /* over all preceding bytes; must stay last */
} smu_telemetry_t;

/* ============================================================
 * Command — SMU_REG_COMMAND, write-only, 16 bytes
 * ============================================================ */

typedef struct __attribute__((packed)) {
    uint8_t  opcode;            /* SMU_CMD_*                               */
    uint8_t  arg8;
    uint16_t arg16;

    /* Echoed back in smu_ident_t so the master can distinguish "command
     * not yet seen" from "seen, and this is its result". Without it a
     * retried command is indistinguishable from a stuck result code. */
    uint32_t nonce;

    uint32_t _reserved;
    uint16_t _pad;
    uint16_t crc16;
} smu_command_t;

/* ============================================================
 * Configuration — SMU_REG_CONFIG, read/write, 296 bytes
 *
 * Two-phase write: the master writes the whole block, then issues
 * SMU_CMD_CONFIG_COMMIT. A partial write interrupted mid-transfer is
 * never applied — the SMU validates the complete staged block before
 * accepting it and rejects with a reason code the UI can surface.
 *
 * Layout mirrors spindle_cfg_t from app_config.h but is redeclared here
 * rather than included: the wire format must stay stable while the
 * internal struct evolves, and the SMU must not inherit the ESP32-only
 * parts of that header.
 * ============================================================ */

typedef struct __attribute__((packed)) {
    float    limit;
    float    hysteresis;        /* engineering units, applied on clearing  */
    uint16_t on_delay_ms;
    uint16_t off_delay_ms;
    uint8_t  enabled;
    uint8_t  latching;
    uint16_t _pad;
} smu_band_cfg_t;

typedef struct __attribute__((packed)) {
    uint16_t schema_version;
    uint8_t  spindle_index;
    uint8_t  flags;             /* SMU_CFG_*                               */

    /* Current channel */
    float    ct_primary_amps;
    float    ct_secondary_ma;
    float    current_gain;
    float    current_zero_offset_v;
    float    noload_cutoff_a;
    uint16_t rms_burst_samples;
    uint8_t  mains_hz;          /* 50 or 60 — sets the RMS burst window    */
    uint8_t  _pad0;

    /* Pressure channel */
    uint8_t  pressure_mode;     /* 0 = 4-20 mA, 1 = 0-10 V                 */
    uint8_t  _pad1[3];
    float    pressure_sensor_min;
    float    pressure_sensor_max;
    float    pressure_gain;
    float    pressure_offset;

    /* RPM channel */
    uint16_t pulses_per_rev;
    uint16_t rpm_zero_timeout_ms;
    uint16_t rpm_glitch_filter_ns;
    uint16_t _pad2;

    /* Arming state machine */
    float    start_rpm;
    float    idle_current_a;
    float    cut_detect_current_a;
    uint16_t settle_ms;
    uint16_t alarm_inhibit_ms;

    /* Wear and transient detectors */
    uint8_t  breakage_enabled;
    uint8_t  breakage_drop_pct;
    uint16_t breakage_window_ms;
    uint8_t  crash_enabled;
    uint8_t  crash_rise_pct;
    uint16_t crash_window_ms;
    uint8_t  trend_enabled;
    uint8_t  trend_cycles;      /* consecutive cycles required to raise    */

    /* Adaptive wear baseline (spec §3.8). The baseline is learned once
     * over baseline_learn_cycles admitted cycles and then FROZEN — a
     * baseline that keeps adapting would track a blunting tool upward and
     * detect nothing. */
    uint8_t  baseline_learn_cycles;      /* 5..64, default 20              */
    uint8_t  baseline_sigma_floor_pct;   /* 1..20, default 2               */
    uint8_t  baseline_sigma_ceiling_pct; /* 5..100, default 25; above this
                                          * the process is too variable and
                                          * the baseline is rejected       */
    uint8_t  _pad3;
    uint16_t _pad3b;
    float    adaptive_k_warn;   /* median + k*sigma, trend warning         */
    float    adaptive_k_alarm;  /* must exceed adaptive_k_warn             */

    /* Output behaviour */
    uint16_t min_pulse_ms;      /* minimum asserted time (DO-R4)           */
    uint16_t _pad4;

    /* [quantity][band] — 192 bytes, and the reason the register pointer
     * has to be 16-bit. */
    smu_band_cfg_t bands[SMU_QTY_COUNT][SMU_BAND_COUNT];

    uint16_t _pad5;
    uint16_t crc16;
} smu_config_t;

/* ============================================================
 * CRC-16/CCITT-FALSE — poly 0x1021, init 0xFFFF, no reflection
 *
 * Chosen over the CRC8 in the original design sketch: the config block
 * is 296 bytes, and CRC8's error-detection over a payload that size is
 * not worth the single byte saved on a link running at 16% utilisation.
 * Calibration corrupted in transit and silently accepted is precisely
 * what this guards against.
 * ============================================================ */

uint16_t smu_crc16(const void *data, uint32_t len);

/* Payload length for a struct whose trailing field is its crc16. */
#define SMU_CRC_PAYLOAD_LEN(type) ((uint32_t)(sizeof(type) - sizeof(uint16_t)))

/* ============================================================
 * Layout enforcement — the point of this header
 *
 * If either compiler pads differently, or a field is added without
 * accounting for it, the build fails here rather than in the field.
 * ============================================================ */

_Static_assert(sizeof(float) == 4, "wire format assumes 32-bit float");

_Static_assert(sizeof(smu_ident_t)     == 16,  "ident must be 16 bytes");
_Static_assert(sizeof(smu_telemetry_t) == 72,  "telemetry must be 72 bytes");
_Static_assert(sizeof(smu_command_t)   == 16,  "command must be 16 bytes");
_Static_assert(sizeof(smu_band_cfg_t)  == 16,  "band cfg must be 16 bytes");
_Static_assert(sizeof(smu_config_t)    == 296, "config must be 296 bytes");

/* Each block must fit inside its window. */
_Static_assert(sizeof(smu_ident_t)     <= SMU_REG_WINDOW,        "ident overruns window");
_Static_assert(sizeof(smu_telemetry_t) <= SMU_REG_WINDOW,        "telemetry overruns window");
_Static_assert(sizeof(smu_command_t)   <= SMU_REG_WINDOW,        "command overruns window");
_Static_assert(sizeof(smu_config_t)    <= SMU_REG_CONFIG_WINDOW, "config overruns window");

/* Windows must not overlap. */
_Static_assert(SMU_REG_IDENT     + SMU_REG_WINDOW <= SMU_REG_TELEMETRY, "ident/telemetry overlap");
_Static_assert(SMU_REG_TELEMETRY + SMU_REG_WINDOW <= SMU_REG_COMMAND,   "telemetry/command overlap");
_Static_assert(SMU_REG_COMMAND   + SMU_REG_WINDOW <= SMU_REG_CONFIG,    "command/config overlap");

/* The bitmaps must be wide enough for the band count they encode. */
_Static_assert(SMU_BAND_COUNT <= 8, "band bitmaps are uint8_t");

#ifdef __cplusplus
}
#endif

#endif /* SMU_PROTO_H */
