/*
 * smu_pack.c — see smu_pack.h.
 *
 * Every field is listed explicitly, in the same order on both sides of
 * each function, specifically so a missing or mismatched field is easy
 * to spot by eye against smu_config_t's own field order in smu_proto.h.
 * Do not "clean this up" into a loop or a table — the whole value of
 * this file being boring and repetitive is that a reviewer can check it
 * field-by-field without also verifying a generic mechanism.
 */

#include "smu_pack.h"

#include <string.h>

/* This file is the one place that sees both quantity_t/band_t
 * (spindle_config.h) and their wire mirrors SMU_QTY_COUNT/SMU_BAND_COUNT
 * (smu_proto.h) at once. A silent mismatch here would turn the band-pack
 * loops below into an out-of-bounds access, so it is checked, not just
 * assumed from the two headers having been written to agree. */
_Static_assert(SMU_QTY_COUNT == QTY_COUNT,
               "smu_proto.h SMU_QTY_COUNT must match spindle_config.h QTY_COUNT");
_Static_assert(SMU_BAND_COUNT == BAND_COUNT,
               "smu_proto.h SMU_BAND_COUNT must match spindle_config.h BAND_COUNT");

static void pack_band(const band_cfg_t *src, smu_band_cfg_t *dst)
{
    dst->limit        = src->limit;
    dst->hysteresis    = src->hysteresis;
    dst->on_delay_ms   = src->on_delay_ms;
    dst->off_delay_ms  = src->off_delay_ms;
    dst->enabled       = src->enabled ? 1U : 0U;
    dst->latching      = src->latching ? 1U : 0U;
}

static void unpack_band(const smu_band_cfg_t *src, band_cfg_t *dst)
{
    dst->limit        = src->limit;
    dst->hysteresis    = src->hysteresis;
    dst->on_delay_ms   = src->on_delay_ms;
    dst->off_delay_ms  = src->off_delay_ms;
    dst->enabled       = (src->enabled != 0);
    dst->latching      = (src->latching != 0);
}

void smu_config_pack(const spindle_cfg_t *src, uint8_t spindle_index,
                     uint16_t schema_version, uint8_t mains_hz,
                     smu_config_t *dst)
{
    memset(dst, 0, sizeof(*dst));

    dst->schema_version = schema_version;
    dst->spindle_index  = spindle_index;
    dst->flags = (uint8_t)
        ((src->enabled                  ? SMU_CFG_SPINDLE_ENABLED     : 0) |
         (src->machine_running_enabled  ? SMU_CFG_MACHINE_RUN_ENABLED : 0));

    /* Current */
    dst->ct_primary_amps      = src->current.ct_primary_amps;
    dst->ct_secondary_ma      = src->current.ct_secondary_ma;
    dst->current_gain         = src->current.gain_correction;
    dst->current_zero_offset_v= src->current.zero_offset_v;
    dst->noload_cutoff_a      = src->current.noload_cutoff_a;
    dst->rms_burst_samples    = src->current.rms_burst_samples;
    dst->mains_hz             = mains_hz;

    /* Pressure. Note: pressure.unit[] (the display string, "bar"/"psi"/
     * ...) deliberately does NOT cross the wire — the SMU's own maths is
     * unit-agnostic given sensor_min/sensor_max, and the unit label is a
     * pure UI display concern that belongs on the ESP32 only. */
    dst->pressure_mode        = (uint8_t)src->pressure.mode;
    dst->pressure_sensor_min  = src->pressure.sensor_min;
    dst->pressure_sensor_max  = src->pressure.sensor_max;
    dst->pressure_gain        = src->pressure.gain_correction;
    dst->pressure_offset      = src->pressure.offset_correction;

    /* RPM */
    dst->pulses_per_rev       = src->rpm.pulses_per_rev;
    dst->rpm_zero_timeout_ms  = src->rpm.zero_timeout_ms;
    dst->rpm_glitch_filter_ns = src->rpm.glitch_filter_ns;

    /* Arming state machine */
    dst->start_rpm            = src->sm.start_rpm;
    dst->idle_current_a       = src->sm.idle_current_a;
    dst->cut_detect_current_a = src->sm.cut_detect_current_a;
    dst->settle_ms            = src->sm.settle_ms;
    dst->alarm_inhibit_ms     = src->sm.alarm_inhibit_ms;

    /* Wear / transient detectors, including the adaptive baseline */
    dst->breakage_enabled            = src->wear.breakage_enabled ? 1U : 0U;
    dst->breakage_drop_pct           = src->wear.breakage_drop_pct;
    dst->breakage_window_ms          = src->wear.breakage_window_ms;
    dst->crash_enabled                = src->wear.crash_enabled ? 1U : 0U;
    dst->crash_rise_pct               = src->wear.crash_rise_pct;
    dst->crash_window_ms              = src->wear.crash_window_ms;
    dst->trend_enabled                = src->wear.trend_enabled ? 1U : 0U;
    dst->trend_cycles                 = src->wear.trend_cycles;
    dst->baseline_learn_cycles        = src->wear.baseline_learn_cycles;
    dst->baseline_sigma_floor_pct     = src->wear.baseline_sigma_floor_pct;
    dst->baseline_sigma_ceiling_pct   = src->wear.baseline_sigma_ceiling_pct;
    dst->adaptive_k_warn              = src->wear.adaptive_k_warn;
    dst->adaptive_k_alarm             = src->wear.adaptive_k_alarm;

    /* Output behaviour */
    dst->min_pulse_ms = src->min_pulse_ms;

    /* Bands, [quantity][band]. Loop variables are unsigned to match
     * SMU_QTY_COUNT/SMU_BAND_COUNT's type — smu_proto.h defines the wire
     * constants as explicit `U`-suffixed #defines rather than enum
     * constants (see its own header comment), so signed loop counters
     * here would compare against an unsigned bound. */
    for (unsigned q = 0; q < SMU_QTY_COUNT; q++) {
        for (unsigned b = 0; b < SMU_BAND_COUNT; b++) {
            pack_band(&src->bands[q][b], &dst->bands[q][b]);
        }
    }

    dst->crc16 = smu_crc16(dst, SMU_CRC_PAYLOAD_LEN(smu_config_t));
}

void smu_config_unpack(const smu_config_t *src, spindle_cfg_t *dst,
                       uint8_t *spindle_index_out, uint8_t *mains_hz_out)
{
    memset(dst, 0, sizeof(*dst));

    /* name[] is not carried on the wire (ESP32-only display label) — left
     * empty here; a caller that wants the operator-assigned name keeps
     * its own copy rather than expecting this function to invent one. */

    dst->enabled                 = (src->flags & SMU_CFG_SPINDLE_ENABLED) != 0;
    dst->machine_running_enabled = (src->flags & SMU_CFG_MACHINE_RUN_ENABLED) != 0;

    if (spindle_index_out) *spindle_index_out = src->spindle_index;
    if (mains_hz_out)      *mains_hz_out      = src->mains_hz;

    /* Current */
    dst->current.ct_primary_amps   = src->ct_primary_amps;
    dst->current.ct_secondary_ma   = src->ct_secondary_ma;
    dst->current.gain_correction   = src->current_gain;
    dst->current.zero_offset_v     = src->current_zero_offset_v;
    dst->current.noload_cutoff_a   = src->noload_cutoff_a;
    dst->current.rms_burst_samples = src->rms_burst_samples;

    /* Pressure */
    dst->pressure.mode              = (pressure_input_mode_t)src->pressure_mode;
    dst->pressure.sensor_min        = src->pressure_sensor_min;
    dst->pressure.sensor_max        = src->pressure_sensor_max;
    dst->pressure.gain_correction   = src->pressure_gain;
    dst->pressure.offset_correction = src->pressure_offset;

    /* RPM */
    dst->rpm.pulses_per_rev   = src->pulses_per_rev;
    dst->rpm.zero_timeout_ms  = src->rpm_zero_timeout_ms;
    dst->rpm.glitch_filter_ns = src->rpm_glitch_filter_ns;

    /* Arming state machine */
    dst->sm.start_rpm            = src->start_rpm;
    dst->sm.idle_current_a       = src->idle_current_a;
    dst->sm.cut_detect_current_a = src->cut_detect_current_a;
    dst->sm.settle_ms            = src->settle_ms;
    dst->sm.alarm_inhibit_ms     = src->alarm_inhibit_ms;

    /* Wear / transient detectors, including the adaptive baseline */
    dst->wear.breakage_enabled            = (src->breakage_enabled != 0);
    dst->wear.breakage_drop_pct           = src->breakage_drop_pct;
    dst->wear.breakage_window_ms          = src->breakage_window_ms;
    dst->wear.crash_enabled                = (src->crash_enabled != 0);
    dst->wear.crash_rise_pct               = src->crash_rise_pct;
    dst->wear.crash_window_ms              = src->crash_window_ms;
    dst->wear.trend_enabled                = (src->trend_enabled != 0);
    dst->wear.trend_cycles                 = src->trend_cycles;
    dst->wear.baseline_learn_cycles        = src->baseline_learn_cycles;
    dst->wear.baseline_sigma_floor_pct     = src->baseline_sigma_floor_pct;
    dst->wear.baseline_sigma_ceiling_pct   = src->baseline_sigma_ceiling_pct;
    dst->wear.adaptive_k_warn              = src->adaptive_k_warn;
    dst->wear.adaptive_k_alarm             = src->adaptive_k_alarm;

    /* Output behaviour */
    dst->min_pulse_ms = src->min_pulse_ms;

    /* Bands, [quantity][band] — see the matching comment in pack() above. */
    for (unsigned q = 0; q < SMU_QTY_COUNT; q++) {
        for (unsigned b = 0; b < SMU_BAND_COUNT; b++) {
            unpack_band(&src->bands[q][b], &dst->bands[q][b]);
        }
    }
}
