/*
 * scaling.c — engineering-unit conversion.
 */

#include "scaling.h"

/* ============================================================
 * Pressure
 * ============================================================ */

/* 4-20 mA fault thresholds, expressed as loop current. NAMUR NE 43 puts
 * the "sensor failed low" boundary at 3.6 mA and the high boundary at
 * 21.0 mA; 3.5 gives a little margin below that for burden tolerance. */
#define LOOP_OPEN_MA        3.5f
#define LOOP_OVER_MA       21.0f

/* 0-10 V mode has no equivalent of a live zero, so a broken wire reads as
 * a genuine 0 V and is indistinguishable from a real zero-pressure
 * reading. This is a property of the signal standard, not of this code —
 * it is the main reason to prefer 4-20 mA on anything that matters. */
#define VOLT_MODE_CEILING  10.5f
#define VOLT_MODE_FLOOR    (-0.2f)

float pressure_loop_ma(float adc_volts)
{
    return (adc_volts / PRESSURE_BURDEN_OHM) * 1000.0f;
}

sensor_status_t pressure_scale(const pressure_cfg_t *cfg, float adc_volts,
                               float *out)
{
    float fraction;   /* 0.0 at sensor_min, 1.0 at sensor_max */

    if (cfg->mode == PRESSURE_INPUT_4_20MA) {
        float ma = pressure_loop_ma(adc_volts);

        if (ma < LOOP_OPEN_MA)  return SENSOR_OPEN;
        if (ma > LOOP_OVER_MA)  return SENSOR_OVERRANGE;

        fraction = (ma - 4.0f) / 16.0f;
    } else {
        /* Undo the input divider to recover the field voltage. */
        float field_v = adc_volts / PRESSURE_DIV_RATIO;

        if (field_v < VOLT_MODE_FLOOR)   return SENSOR_UNDERRANGE;
        if (field_v > VOLT_MODE_CEILING) return SENSOR_OVERRANGE;

        fraction = field_v / 10.0f;
    }

    /* Allow a little excursion past the calibrated ends. A transmitter
     * sitting at 3.9 mA is within tolerance of zero, and clamping the
     * fraction to [0,1] would hide a real negative offset from whoever is
     * trying to calibrate the thing. */
    if (fraction < -0.05f) fraction = -0.05f;
    if (fraction >  1.05f) fraction =  1.05f;

    float span = cfg->sensor_max - cfg->sensor_min;
    float value = cfg->sensor_min + fraction * span;

    *out = value * cfg->gain_correction + cfg->offset_correction;
    return SENSOR_OK;
}

/* ============================================================
 * Current
 * ============================================================ */

float current_scale(const current_cfg_t *cfg, float burden_vrms)
{
    /* Secondary RMS current implied by the voltage across the burden. */
    float secondary_a = burden_vrms / CT_BURDEN_OHM;

    /* Scale by the CT ratio. ct_secondary_ma is the secondary output at
     * rated primary current, so the ratio is primary/secondary. */
    float secondary_rated_a = cfg->ct_secondary_ma / 1000.0f;
    if (secondary_rated_a <= 0.0f) return 0.0f;   /* validation should prevent this */

    float ratio = cfg->ct_primary_amps / secondary_rated_a;
    float amps = secondary_a * ratio * cfg->gain_correction;

    /* Current is a magnitude; a negative result means the zero reference
     * drifted above the signal, which is a calibration problem rather than
     * a negative current. Report zero and let auto-zero fix it. */
    if (amps <= 0.0f) return 0.0f;

    /* No-load deadband. Suppress rather than subtract: a reading just above
     * the threshold must keep its true magnitude, otherwise every value in
     * the working range would be shifted down by the cutoff and the CT
     * calibration would be quietly wrong everywhere. */
    if (amps < cfg->noload_cutoff_a) return 0.0f;

    return amps;
}

/* ============================================================
 * RPM
 * ============================================================ */

float rpm_from_interval(float interval_us, unsigned pulses_per_rev)
{
    if (interval_us <= 0.0f || pulses_per_rev == 0) return 0.0f;

    /* revs/sec = 1e6 / (interval_us * ppr);  rpm = that * 60 */
    return 60.0e6f / (interval_us * (float)pulses_per_rev);
}

float rpm_from_count(unsigned pulses, float gate_ms, unsigned pulses_per_rev)
{
    if (gate_ms <= 0.0f || pulses_per_rev == 0) return 0.0f;

    float revs = (float)pulses / (float)pulses_per_rev;
    float minutes = gate_ms / 60000.0f;
    return revs / minutes;
}
