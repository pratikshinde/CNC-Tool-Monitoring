/*
 * job_template.c — see job_template.h.
 */
#include "job_template.h"

#include <string.h>

/* ============================================================
 * Extract
 * ============================================================ */

void job_profile_extract(const spindle_cfg_t *src, job_profile_t *dst)
{
    memset(dst, 0, sizeof(*dst));

    dst->enabled                 = src->enabled;
    dst->machine_running_enabled = src->machine_running_enabled;
    dst->min_pulse_ms            = src->min_pulse_ms;
    dst->sm                      = src->sm;
    dst->wear                    = src->wear;
    memcpy(dst->bands, src->bands, sizeof(dst->bands));
}

void job_context_extract(const spindle_cfg_t *src, job_context_t *dst)
{
    memset(dst, 0, sizeof(*dst));

    /* Bounded copy: pressure.unit is a fixed char[CFG_UNIT_LEN] that is not
     * guaranteed NUL-terminated if something ever wrote exactly CFG_UNIT_LEN
     * bytes into it, and dst is the same size. */
    memcpy(dst->pressure_unit, src->pressure.unit, CFG_UNIT_LEN);
    dst->pressure_unit[CFG_UNIT_LEN - 1] = '\0';

    dst->pressure_sensor_min = src->pressure.sensor_min;
    dst->pressure_sensor_max = src->pressure.sensor_max;
    dst->ct_primary_amps     = src->current.ct_primary_amps;
}

/* ============================================================
 * Apply — the enforcement point, see job_template.h's header
 *
 * Every assignment below is a JOB field. There is deliberately no assignment
 * to dst->name, dst->current.*, dst->pressure.* or dst->rpm.* — those are
 * MACHINE fields and must survive an apply untouched. job_profile_t has no
 * member that could carry them even if someone tried.
 * ============================================================ */

void job_profile_apply(const job_profile_t *src, spindle_cfg_t *dst)
{
    dst->enabled                 = src->enabled;
    dst->machine_running_enabled = src->machine_running_enabled;
    dst->min_pulse_ms            = src->min_pulse_ms;
    dst->sm                      = src->sm;
    dst->wear                    = src->wear;
    memcpy(dst->bands, src->bands, sizeof(dst->bands));
}

/* ============================================================
 * Check
 * ============================================================ */

static bool band_is_high(band_t b)
{
    return (b == BAND_HI || b == BAND_HIHI);
}

static void warn_push(job_warnings_t *w, job_warn_kind_t kind, quantity_t q,
                      band_t b, float limit, float machine_max)
{
    if (!w) return;
    if (w->count >= JOB_MAX_WARNINGS) {
        w->truncated = true;
        return;
    }
    job_warning_t *it = &w->item[w->count++];
    it->kind        = kind;
    it->qty         = q;
    it->band        = b;
    it->limit       = limit;
    it->machine_max = machine_max;
}

/* Does this band's limit sit outside what the machine can measure, and if so
 * which way does that fail?
 *
 * A high band (Hi/HiHi) above the measurable ceiling can never be crossed, so
 * it never trips. A high band below the measurable floor is always crossed, so
 * it trips permanently. Low bands (LoLo/Lo) invert both. Both outcomes are
 * worth telling the operator about, and they are genuinely different problems
 * — "this band does nothing" vs "this band screams constantly" — so they get
 * distinct warning kinds rather than a vague "out of range". */
static void check_band_range(job_warnings_t *w, quantity_t q, band_t b,
                             const band_cfg_t *cfg, float meas_min, float meas_max)
{
    if (!cfg->enabled) return;   /* a stale value on a disabled band is fine —
                                  * the UI already labels it "ignored" */

    if (band_is_high(b)) {
        if (cfg->limit > meas_max) {
            warn_push(w, JOB_WARN_NEVER_TRIPS, q, b, cfg->limit, meas_max);
        } else if (cfg->limit < meas_min) {
            warn_push(w, JOB_WARN_ALWAYS_TRIPS, q, b, cfg->limit, meas_max);
        }
    } else {
        if (cfg->limit < meas_min) {
            warn_push(w, JOB_WARN_NEVER_TRIPS, q, b, cfg->limit, meas_max);
        } else if (cfg->limit > meas_max) {
            warn_push(w, JOB_WARN_ALWAYS_TRIPS, q, b, cfg->limit, meas_max);
        }
    }
}

job_check_result_t job_check(const job_profile_t *profile,
                             const job_context_t *ctx,
                             const spindle_cfg_t *machine,
                             uint16_t template_schema_version,
                             uint16_t firmware_schema_version,
                             job_warnings_t *warn)
{
    if (warn) memset(warn, 0, sizeof(*warn));

    if (template_schema_version > firmware_schema_version) {
        return JOB_ERR_SCHEMA_NEWER;
    }

    /* Unit mismatch is a HARD refusal, not a warning: the numbers stay
     * plausible and the error is in the permissive direction. A 60 bar
     * pressure alarm reinterpreted as 60 psi sits at roughly 4 bar of real
     * pressure — the band looks configured, reads sane, and protects
     * nothing. ctx == NULL means a same-machine copy, where by construction
     * both sides share the unit. */
    if (ctx != NULL) {
        if (strncmp(ctx->pressure_unit, machine->pressure.unit, CFG_UNIT_LEN) != 0) {
            return JOB_ERR_UNIT_MISMATCH;
        }
    }

    /* Current: an RMS is never negative, and the CT cannot be trusted above
     * its rated primary, so [0, ct_primary_amps] is the measurable window. */
    for (int b = 0; b < BAND_COUNT; b++) {
        check_band_range(warn, QTY_CURRENT, (band_t)b,
                         &profile->bands[QTY_CURRENT][b],
                         0.0f, machine->current.ct_primary_amps);
    }

    /* Pressure: the configured sensor span on THIS machine. */
    for (int b = 0; b < BAND_COUNT; b++) {
        check_band_range(warn, QTY_PRESSURE, (band_t)b,
                         &profile->bands[QTY_PRESSURE][b],
                         machine->pressure.sensor_min, machine->pressure.sensor_max);
    }

    /* RPM is deliberately not range-checked: nothing in spindle_cfg_t bounds
     * how fast a spindle can turn (pulses_per_rev is a scaling factor, not a
     * ceiling), so there is no honest number to compare against. Inventing a
     * plausible-looking limit here would produce warnings with no basis. */

    return JOB_OK;
}

/* ============================================================
 * Strings
 * ============================================================ */

const char *job_check_result_str(job_check_result_t r)
{
    switch (r) {
    case JOB_OK:                  return "ok";
    case JOB_ERR_UNIT_MISMATCH:   return "pressure unit mismatch";
    case JOB_ERR_SCHEMA_NEWER:    return "template is newer than this firmware";
    default:                      return "?";
    }
}

const char *job_warn_kind_str(job_warn_kind_t k)
{
    switch (k) {
    case JOB_WARN_NEVER_TRIPS:  return "never trips";
    case JOB_WARN_ALWAYS_TRIPS: return "always trips";
    default:                    return "?";
    }
}
