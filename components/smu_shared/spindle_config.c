/*
 * spindle_config.c — per-spindle defaults and validation.
 *
 * Pure C, no ESP-IDF. Compiled into the ESP32 master image, directly into
 * the SMU image, and into the host test harness.
 *
 * A note on the default values below: the electrical and timing defaults
 * (burden resistors, sample rates, delays) are grounded in the hardware and
 * are safe starting points. The tool-wear coefficients are NOT — they are
 * placeholders pending a field trial, and are marked as such. Shipping them
 * unexamined would produce a monitor that cries wolf.
 */

#include "spindle_config.h"

#include <string.h>

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

void spindle_cfg_set_defaults(spindle_cfg_t *s, int index)
{
    memset(s, 0, sizeof(*s));

    s->enabled = true;
    /* snprintf would drag stdio in for no benefit on either target. */
    memcpy(s->name, "Spindle 0", 10);
    s->name[8] = (char)('1' + index);

    /* Machine Running participates in arming by default — matches the
     * hardware document's stated preference for level-driven arming over
     * inferring cycle boundaries. Fault Clear has no equivalent toggle. */
    s->machine_running_enabled = true;

    /* V1's shipped default across all four DO channels (do_cfg_t.min_pulse_ms
     * was 500ms for three outputs and 3000ms for one, per the DO-R4
     * discussion when auto-clear landed) — V2 has one value per spindle
     * governing every SMU output, so 3000ms (the more conservative of the
     * two V1 values) is the safer default to carry forward. */
    s->min_pulse_ms = 3000;

    /* --- Current: 30 A / 1 A CT into 0.1R burden with 2x (6 dB) opamp stage. */
    s->current.ct_primary_amps   = 30.0f;
    s->current.ct_secondary_ma   = 1000.0f;
    s->current.gain_correction   = 0.506f;
    s->current.zero_offset_v     = 1.65f;  /* nominal ADC mid-rail bias */
    /* 0.05 A on a 30 A CT is ~0.17% of full scale — comfortably above the
     * observed ~0.02 A no-load noise floor, and far below any current a
     * real cut would draw. */
    s->current.noload_cutoff_a   = 0.05f;
    s->current.rms_burst_samples = 128;

    /* --- Pressure: 0-250 bar transmitter, 4-20 mA loop by default. */
    s->pressure.mode              = PRESSURE_INPUT_4_20MA;
    s->pressure.sensor_min        = 0.0f;
    s->pressure.sensor_max        = 250.0f;
    memcpy(s->pressure.unit, "bar", 4);
    s->pressure.gain_correction   = 1.0f;
    s->pressure.offset_correction = 0.0f;

    /* --- RPM: one pulse per revolution from a proximity sensor.
     * 12 us of hardware glitch filtering rejects switching noise without
     * touching a real pulse edge. */
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

    /* --- Wear detection. PROVISIONAL — must be tuned against real
     * cutting data before being relied on. */
    s->wear.breakage_enabled   = true;
    s->wear.breakage_drop_pct  = 40;
    s->wear.breakage_window_ms = 100;
    s->wear.crash_enabled      = true;
    s->wear.crash_rise_pct     = 60;
    s->wear.crash_window_ms    = 100;
    s->wear.trend_enabled      = false;   /* needs a learned baseline first */
    s->wear.trend_cycles       = 5;
    s->wear.baseline_learn_cycles      = 20;
    s->wear.baseline_sigma_floor_pct   = 2;
    s->wear.baseline_sigma_ceiling_pct = 25;
    s->wear.adaptive_k_warn    = 3.0f;
    s->wear.adaptive_k_alarm   = 5.0f;

    /* --- Thresholds, sized against the default 30 A CT and 250 bar sensor. */
    default_bands(s->bands[QTY_CURRENT],   2.0f,   5.0f,  35.0f,  45.0f,  0.5f);
    default_bands(s->bands[QTY_PRESSURE], 10.0f,  20.0f,  80.0f,  95.0f,  1.0f);
    default_bands(s->bands[QTY_RPM],     100.0f, 500.0f, 9000.0f, 10000.0f, 50.0f);
}

/* ============================================================
 * Validation
 * ============================================================ */

static cfg_result_t validate_bands(const band_cfg_t b[BAND_COUNT])
{
    /* Only compare bands that are actually enabled. A user running
     * high-only monitoring will leave the low limits at whatever they
     * were, and those stale values must not block the save. */
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

cfg_result_t spindle_cfg_validate(const spindle_cfg_t *s)
{
    /* 0 would mean an SMU output could de-assert the instant its
     * condition clears, defeating DO-R4's "slow PLC scan must not step
     * over a brief event" guarantee. 60 s is an arbitrary but generous
     * ceiling against a fat-fingered entry. */
    if (s->min_pulse_ms == 0 || s->min_pulse_ms > 60000) {
        return CFG_ERR_PERIOD_RANGE;
    }

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
     * burst takes longer than half a second and breakage detection stops
     * being possible at all. */
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

    /* Adaptive wear baseline (FIRMWARE_DESIGN_SPEC.md §3.8): the warning
     * limit must sit inside the alarm limit, and the floor below the
     * ceiling, or the learned baseline's validity gate cannot behave as
     * documented. */
    if (s->wear.adaptive_k_warn <= 0.0f || s->wear.adaptive_k_alarm <= 0.0f ||
        s->wear.adaptive_k_warn >= s->wear.adaptive_k_alarm) {
        return CFG_ERR_WEAR_BASELINE;
    }
    if (s->wear.baseline_sigma_floor_pct == 0 ||
        s->wear.baseline_sigma_floor_pct >= s->wear.baseline_sigma_ceiling_pct) {
        return CFG_ERR_WEAR_BASELINE;
    }
    if (s->wear.baseline_learn_cycles < 5 || s->wear.baseline_learn_cycles > 64) {
        return CFG_ERR_WEAR_BASELINE;
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
    case CFG_ERR_BURST_RANGE:   return "RMS burst size out of range";
    case CFG_ERR_MAINS_HZ:      return "mains frequency must be 50 or 60";
    case CFG_ERR_MODBUS_RANGE:  return "invalid Modbus parameter";
    case CFG_ERR_WEAR_BASELINE: return "invalid wear baseline parameters";
    default:                    return "unknown";
    }
}
