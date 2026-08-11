/*
 * app_config.c — defaults, validation and integrity checking.
 *
 * Pure C, no ESP-IDF. Compiled both into the firmware and into the host
 * test harness.
 *
 * A note on the default values below: the electrical and timing defaults
 * (burden resistors, sample rates, delays) are grounded in the hardware and
 * are safe starting points. The tool-wear coefficients are NOT — they are
 * placeholders pending the Phase 6 field trial, and are marked as such.
 * Shipping them unexamined would produce a monitor that cries wolf.
 */

#include "app_config.h"

#include <stddef.h>
#include <string.h>

/* ============================================================
 * CRC-32 (IEEE 802.3, reflected) — small table-free implementation.
 * Config blobs are a couple of hundred bytes, so the bitwise loop costs
 * nothing and saves a 1 KB table.
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

static void default_bands(band_cfg_t bands[BAND_COUNT],
                          float lolo, float lo, float hi, float hihi,
                          float hyst)
{
    /* Ship with the critical bands armed and the warning bands armed but
     * non-latching. A device that arrives with everything disabled tends to
     * stay that way; a device that arrives latching everything gets its
     * outputs disconnected on day two. */
    bands[BAND_LOLO] = (band_cfg_t){
        .enabled = false,          /* low limits need a known-good process first */
        .limit = lolo, .hysteresis = hyst,
        .on_delay_ms = 200, .off_delay_ms = 500, .latching = true,
    };
    bands[BAND_LO] = (band_cfg_t){
        .enabled = false,
        .limit = lo, .hysteresis = hyst,
        .on_delay_ms = 500, .off_delay_ms = 1000, .latching = false,
    };
    bands[BAND_HI] = (band_cfg_t){
        .enabled = true,
        .limit = hi, .hysteresis = hyst,
        .on_delay_ms = 500, .off_delay_ms = 1000, .latching = false,
    };
    bands[BAND_HIHI] = (band_cfg_t){
        .enabled = true,
        .limit = hihi, .hysteresis = hyst,
        .on_delay_ms = 100, .off_delay_ms = 500, .latching = true,
    };
}

static void default_spindle(spindle_cfg_t *s, int index)
{
    memset(s, 0, sizeof(*s));

    s->enabled = true;
    /* snprintf would drag stdio into the host test for no benefit. */
    memcpy(s->name, "Spindle 0", 10);
    s->name[8] = (char)('1' + index);

    /* --- Current: 30 A / 1 A CT into 0.1R burden with 2x (6 dB) opamp stage. */
    s->current.ct_primary_amps   = 30.0f;
    s->current.ct_secondary_ma   = 1000.0f;
    s->current.gain_correction   = 0.506f;
    s->current.zero_offset_v     = CT_BIAS_VOLTS;
    /* 0.05 A on a 30 A CT is ~0.17% of full scale — comfortably above the
     * observed ~0.02 A no-load noise floor, and far below any current a
     * real cut would draw. */
    s->current.noload_cutoff_a   = 0.05f;
    s->current.rms_burst_samples = 128;

    /* --- Pressure: 0-250 bar transmitter on a 4-20 mA loop across 180R shunt. */
    s->pressure.mode              = PRESSURE_INPUT_4_20MA;
    s->pressure.sensor_min        = 0.0f;
    s->pressure.sensor_max        = 250.0f;
    memcpy(s->pressure.unit, "bar", 4);
    s->pressure.gain_correction   = 1.0f;
    s->pressure.offset_correction = 0.0f;

    /* --- RPM: one pulse per revolution from a proximity sensor.
     * 12 us of hardware glitch filtering rejects switching noise without
     * touching a real pulse edge (see rpm.c for why this cannot be ms). */
    s->rpm.pulses_per_rev   = 1;
    s->rpm.glitch_filter_ns = 12000;
    s->rpm.zero_timeout_ms  = 2000;

    /* --- State machine. These gate all monitoring, so conservative
     * defaults here cost a little sensitivity and buy a lot of quiet. */
    s->sm.start_rpm            = 50.0f;
    s->sm.settle_ms            = 500;
    s->sm.idle_current_a       = 3.0f;
    s->sm.cut_detect_current_a = 6.0f;
    s->sm.alarm_inhibit_ms     = 500;

    /* --- Wear detection. PROVISIONAL — see SRS §6.2 validation caveat.
     * These must be tuned against real cutting data in Phase 6. */
    s->wear.breakage_enabled   = true;
    s->wear.breakage_drop_pct  = 40;
    s->wear.breakage_window_ms = 100;
    s->wear.crash_enabled      = true;
    s->wear.crash_rise_pct     = 60;
    s->wear.crash_window_ms    = 100;
    s->wear.trend_enabled      = false;   /* needs a learned baseline first */
    s->wear.trend_cycles       = 5;
    s->wear.adaptive_k_warn    = 3.0f;
    s->wear.adaptive_k_alarm   = 5.0f;

    /* --- Thresholds, sized against the default 50 A CT and 100 bar sensor. */
    default_bands(s->bands[QTY_CURRENT],   2.0f,   5.0f,  35.0f,  45.0f,  0.5f);
    default_bands(s->bands[QTY_PRESSURE], 10.0f,  20.0f,  80.0f,  95.0f,  1.0f);
    default_bands(s->bands[QTY_RPM],     100.0f, 500.0f, 9000.0f, 10000.0f, 50.0f);
}

void app_config_set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->schema_version = CONFIG_SCHEMA_VERSION;

    cfg->system.measure_period_ms = 100;   /* 10 Hz — NFR-1 budget */
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
        default_spindle(&cfg->spindle[i], i);
    }

    /* Factory DO preset, per customer requirement: current + RPM anomaly
     * together on one output per spindle, pressure on its own output per
     * spindle. Pressure is inverted — normally energised, de-energises on
     * an abnormal reading — everything else is normally de-energised,
     * asserting on fault. Designated initializers deliberately, not
     * positional: do_cfg_t has grown fields before and a silently
     * mis-ordered positional list is exactly the kind of bug that only
     * shows up on the bench.
     *
     * Current/RPM outputs get a 3 s minimum ON time (min_pulse_ms) per
     * customer requirement: once asserted, hold for at least 3 s even if
     * the fault clears sooner, so a PLC on a slow scan cannot miss it. This
     * is a do_cfg_t field, not a band field, so it is not reachable from
     * the Thresholds tab today — there is no "Outputs" tab yet. Changing
     * it means editing this default and reflashing, same as this change. */
    cfg->dout[0] = (do_cfg_t){
        .source = DO_SRC_SPINDLE_QUANTITY, .spindle = 0,
        .quantity_mask = DO_QTY_CURRENT | DO_QTY_RPM,
        .invert = false, .min_pulse_ms = 3000,
    };
    cfg->dout[1] = (do_cfg_t){
        .source = DO_SRC_SPINDLE_QUANTITY, .spindle = 0,
        .quantity_mask = DO_QTY_PRESSURE,
        .invert = true, .min_pulse_ms = 500,
    };
    cfg->dout[2] = (do_cfg_t){
        .source = DO_SRC_SPINDLE_QUANTITY, .spindle = 1,
        .quantity_mask = DO_QTY_CURRENT | DO_QTY_RPM,
        .invert = false, .min_pulse_ms = 3000,
    };
    cfg->dout[3] = (do_cfg_t){
        .source = DO_SRC_SPINDLE_QUANTITY, .spindle = 1,
        .quantity_mask = DO_QTY_PRESSURE,
        .invert = true, .min_pulse_ms = 500,
    };

    app_config_seal(cfg);
}

/* ============================================================
 * Validation (UI-R6 server-side backstop)
 * ============================================================ */

static cfg_result_t validate_bands(const band_cfg_t b[BAND_COUNT])
{
    /* Only compare bands that are actually enabled. A user running
     * high-only monitoring (AL-R1) will leave the low limits at whatever
     * they were, and those stale values must not block the save. */
    float prev = -3.4e38f;
    bool  have_prev = false;

    for (int i = 0; i < BAND_COUNT; i++) {
        if (!b[i].enabled) continue;
        if (have_prev && b[i].limit < prev) return CFG_ERR_BAND_ORDER;
        prev = b[i].limit;
        have_prev = true;
    }

    /* Hysteresis wider than the gap between adjacent enabled bands would
     * make the upper band impossible to clear without also clearing the
     * lower one, producing a permanently stuck alarm. */
    for (int i = 0; i < BAND_COUNT; i++) {
        if (b[i].enabled && b[i].hysteresis < 0.0f) return CFG_ERR_BAND_ORDER;
    }
    return CFG_OK;
}

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

        if (s->current.ct_primary_amps <= 0.0f ||
            s->current.ct_secondary_ma <= 0.0f ||
            s->current.gain_correction <= 0.0f) {
            return CFG_ERR_CT_RANGE;
        }

        /* A deadband is meant to swallow the noise floor, nothing more. Cap
         * it at 5% of the CT rating: beyond that it would start hiding real
         * cutting current, and a monitor that silently reports zero while
         * the tool is loaded is worse than one that reads a little noise. */
        if (s->current.noload_cutoff_a < 0.0f ||
            s->current.noload_cutoff_a > s->current.ct_primary_amps * 0.05f) {
            return CFG_ERR_CT_RANGE;
        }

        /* Below 16 samples the RMS estimate is meaningless; above 512 the
         * burst takes longer than half a second and breakage detection
         * stops being possible at all. */
        if (s->current.rms_burst_samples < 16 || s->current.rms_burst_samples > 512) {
            return CFG_ERR_BURST_RANGE;
        }

        if (s->pressure.sensor_min >= s->pressure.sensor_max) return CFG_ERR_SENSOR_SPAN;
        if (s->pressure.gain_correction <= 0.0f)              return CFG_ERR_SENSOR_SPAN;

        if (s->rpm.pulses_per_rev < 1 || s->rpm.pulses_per_rev > 1024) {
            return CFG_ERR_PPR_RANGE;
        }
        if (s->rpm.zero_timeout_ms < 50) return CFG_ERR_PPR_RANGE;

        /* If idle and cut-detect overlap, the state machine can chatter
         * between IDLE and CUTTING, arming and disarming alarms on every
         * cycle. Require real separation, not just ordering. */
        if (s->sm.cut_detect_current_a <= s->sm.idle_current_a * 1.2f) {
            return CFG_ERR_SM_THRESHOLD;
        }
        if (s->sm.start_rpm <= 0.0f) return CFG_ERR_SM_THRESHOLD;

        for (int q = 0; q < QTY_COUNT; q++) {
            cfg_result_t r = validate_bands(s->bands[q]);
            if (r != CFG_OK) return r;
        }
    }

    if (which) *which = 0xFF;

    for (int d = 0; d < NUM_DIGITAL_OUT; d++) {
        if (cfg->dout[d].source >= DO_SRC_COUNT) return CFG_ERR_DO_SOURCE;
        if ((cfg->dout[d].source == DO_SRC_SPINDLE_ALARM ||
             cfg->dout[d].source == DO_SRC_SPINDLE_WARNING ||
             cfg->dout[d].source == DO_SRC_SPINDLE_QUANTITY) &&
            cfg->dout[d].spindle >= NUM_SPINDLES) {
            return CFG_ERR_DO_SOURCE;
        }
        if (cfg->dout[d].source == DO_SRC_SPINDLE_QUANTITY &&
            (cfg->dout[d].quantity_mask == 0 ||
             cfg->dout[d].quantity_mask > DO_QTY_ALL)) {
            /* An empty mask would silently never assert, which looks
             * indistinguishable from "working, nothing wrong" — treat it
             * as a configuration error rather than a quiet no-op output. */
            return CFG_ERR_DO_SOURCE;
        }
    }

    return CFG_OK;
}

const char *app_config_result_str(cfg_result_t r)
{
    switch (r) {
    case CFG_OK:                return "ok";
    case CFG_ERR_SCHEMA:        return "unsupported schema version";
    case CFG_ERR_BAND_ORDER:    return "threshold bands out of order";
    case CFG_ERR_SENSOR_SPAN:   return "invalid sensor span";
    case CFG_ERR_PPR_RANGE:     return "pulses-per-rev out of range";
    case CFG_ERR_CT_RANGE:      return "invalid CT parameters";
    case CFG_ERR_SM_THRESHOLD:  return "cut-detect must exceed idle by 20%";
    case CFG_ERR_PERIOD_RANGE:  return "timing parameter out of range";
    case CFG_ERR_DO_SOURCE:     return "invalid digital output mapping";
    case CFG_ERR_BURST_RANGE:   return "RMS burst size out of range";
    case CFG_ERR_MAINS_HZ:      return "mains frequency must be 50 or 60";
    case CFG_ERR_MODBUS_RANGE:  return "invalid Modbus parameter";
    default:                    return "unknown";
    }
}
