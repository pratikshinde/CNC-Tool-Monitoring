/*
 * job_template.h — the job/machine split that makes a saved job portable.
 *
 * A customer runs many different jobs on one machine. Every job change used to
 * mean re-typing every threshold by hand. A job template captures those
 * settings once and reloads them in a single action — including onto a
 * DIFFERENT machine, so a job proven on line 1 can be carried to line 2.
 *
 * ============================================================
 * Why this file exists at all
 * ============================================================
 *
 * spindle_cfg_t (spindle_config.h) mixes two kinds of data that look alike and
 * are not:
 *
 *   JOB data     — belongs to "Acme bracket, op 20". Thresholds, cut-detect
 *                  levels, wear/breakage settings. Meaningful on any machine.
 *   MACHINE data — belongs to THIS machine and THESE sensors. Gain
 *                  corrections, the auto-zero tare, CT ratio, sensor range,
 *                  pulses-per-rev.
 *
 * Copying a whole spindle_cfg_t between machines (or even between the two
 * spindles of one machine — each has its own CT and its own pressure sensor)
 * overwrites the destination's calibration with the source's. Nothing breaks
 * loudly: the device still runs, every diagnostic still passes, every band
 * still evaluates. The readings are simply wrong, by the ratio between the two
 * calibrations, forever. That is precisely the silent-wrongness failure class
 * this product exists to prevent.
 *
 * So: job_profile_t holds ONLY the job half. It has no field for a gain
 * correction, so a template structurally cannot carry one — the guarantee is
 * enforced by the type, not by remembering to be careful in the copy code.
 *
 * job_profile_apply() is the one place job data crosses into a live
 * spindle_cfg_t. If a machine field is ever added to job_profile_t or written
 * by that function, calibration starts travelling again and nothing will look
 * broken. host_test's round-trip case (asserting every machine field is
 * byte-identical after an apply) is the guard on that. Do not delete it.
 *
 * Deliberately free of cJSON, LittleFS and ESP-IDF: the logic worth testing is
 * the split and the compatibility check, and both run under host_test/ with no
 * hardware. Serialization and file storage live in job_store.[ch].
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "spindle_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * The job half — everything a template carries and applies
 * ============================================================ */

typedef struct {
    /* Whether this spindle participates in the job at all. Job data, not
     * machine data: a single-spindle job legitimately leaves the other side
     * disabled, and unlike calibration this is loudly visible in the UI
     * rather than silently skewing a reading. */
    bool             enabled;

    bool             machine_running_enabled;
    uint16_t         min_pulse_ms;

    spindle_sm_cfg_t sm;     /* cut detection — start_rpm, idle, cut-detect… */
    wear_cfg_t       wear;   /* breakage / crash / trend detectors            */

    band_cfg_t       bands[QTY_COUNT][BAND_COUNT];
} job_profile_t;

/* ============================================================
 * The machine context a profile was authored against
 *
 * Recorded so an import can be CHECKED against the destination, never so it
 * can be applied. job_profile_apply() cannot write any of this — it is not
 * reachable from job_profile_t.
 * ============================================================ */

typedef struct {
    char  pressure_unit[CFG_UNIT_LEN];
    float pressure_sensor_min;
    float pressure_sensor_max;
    float ct_primary_amps;
} job_context_t;

/* ============================================================
 * Compatibility check results
 * ============================================================ */

typedef enum {
    JOB_OK = 0,
    /* A 60 "bar" limit applied on a psi machine is wrong in the PERMISSIVE
     * direction (60 psi ~= 4 bar), so this is a hard refusal, not a warning. */
    JOB_ERR_UNIT_MISMATCH,
    /* Template written by newer firmware than this build understands. */
    JOB_ERR_SCHEMA_NEWER,
} job_check_result_t;

typedef enum {
    /* Limit lies outside what this machine's hardware can ever measure, in
     * the direction the band watches — the band is decoration. An operator
     * who believes a band protects them when it physically cannot trip is
     * worse off than one with no band at all. */
    JOB_WARN_NEVER_TRIPS,
    /* The opposite: the measured value is permanently on the tripped side of
     * this limit, so the band will assert immediately and forever. */
    JOB_WARN_ALWAYS_TRIPS,
} job_warn_kind_t;

typedef struct {
    job_warn_kind_t kind;
    quantity_t      qty;
    band_t          band;
    float           limit;       /* what the template asked for            */
    float           machine_max; /* what this machine can actually reach   */
} job_warning_t;

/* 12 bands exist (3 quantities x 4), but a template that trips more than a
 * handful of these is misconfigured in a way no list length will fix — the
 * cap keeps this struct small enough to sit on the HTTP handler's stack. */
#define JOB_MAX_WARNINGS 12

typedef struct {
    uint8_t       count;
    bool          truncated;   /* more warnings existed than fit */
    job_warning_t item[JOB_MAX_WARNINGS];
} job_warnings_t;

/* ============================================================
 * API — all pure, no I/O
 * ============================================================ */

/* Pull the job half out of a live spindle config. */
void job_profile_extract(const spindle_cfg_t *src, job_profile_t *dst);

/* Record the machine context the profile was authored against. */
void job_context_extract(const spindle_cfg_t *src, job_context_t *dst);

/* Write the job half into a working copy of a spindle config, leaving every
 * machine field exactly as `dst` already had it. THE enforcement point of
 * this whole feature — see the file header. */
void job_profile_apply(const job_profile_t *src, spindle_cfg_t *dst);

/* Check a profile against the machine it is about to be applied to.
 *
 * Returns JOB_OK, or a hard error the caller must refuse on. Warnings are
 * collected into *warn regardless (they never block on their own — the
 * product decision is warn-and-apply, made safe by the caller separately
 * refusing to apply anything at all while the spindle is CUTTING, so a human
 * is always present at a stopped machine when a warning appears).
 *
 * `ctx` may be NULL for a same-machine copy, where there is no authored
 * context to compare — the reachability warnings are still produced against
 * `machine`.
 *
 * Both schema versions are passed in rather than read from app_config.h's
 * CONFIG_SCHEMA_VERSION: that header is the ESP32-only root config, and this
 * file stays free of it so the whole check runs under host_test/. */
job_check_result_t job_check(const job_profile_t *profile,
                             const job_context_t *ctx,
                             const spindle_cfg_t *machine,
                             uint16_t template_schema_version,
                             uint16_t firmware_schema_version,
                             job_warnings_t *warn);

const char *job_check_result_str(job_check_result_t r);
const char *job_warn_kind_str(job_warn_kind_t k);

#ifdef __cplusplus
}
#endif
