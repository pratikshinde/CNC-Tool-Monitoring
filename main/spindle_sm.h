/*
 * spindle_sm.h — per-spindle operating state machine (SRS §5.1).
 *
 * This is the most important piece of false-alarm suppression in the
 * product. Spindle start-up draws several times the cutting current, and
 * an idle spindle at speed still draws windage. If thresholds were
 * evaluated unconditionally, every single cycle would trip an alarm, the
 * operator would disconnect the outputs within a week, and the device
 * would be worthless. Monitoring is therefore armed only in CUTTING.
 *
 * Pure C, no ESP-IDF: time is passed in, not read. That makes the whole
 * state machine deterministic and testable on a host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPINDLE_STOPPED = 0,
    SPINDLE_SPIN_UP,
    SPINDLE_IDLE,
    SPINDLE_CUTTING,
    SPINDLE_COAST_DOWN,
    SPINDLE_STATE_COUNT
} spindle_state_t;

typedef struct {
    spindle_state_t state;
    int64_t         state_since_us;
    int64_t         cut_started_us;   /* valid while CUTTING */

    /* True once the post-transition inhibit window has expired, i.e. it is
     * safe to act on threshold violations. */
    bool            monitoring_armed;

    /* Set for exactly one call when a cut ends, so the caller can close
     * out a cycle summary record. */
    bool            cycle_completed;
    uint32_t        last_cycle_ms;

    /* Running aggregates over the current cut. */
    float           cut_current_sum;
    float           cut_current_peak;
    float           cut_pressure_sum;
    uint32_t        cut_samples;

    /* Cumulative count of completed cuts, for the cycle log. */
    uint32_t        cycle_count;
} spindle_sm_t;

/* Summary of one completed cut, valid when cycle_completed is set. */
typedef struct {
    uint32_t duration_ms;
    float    mean_current;
    float    peak_current;
    float    mean_pressure;
    uint32_t samples;
} cycle_summary_t;

void spindle_sm_init(spindle_sm_t *sm, int64_t now_us);

/* Advance the machine by one measurement sample.
 * `rpm_stopped` comes from the RPM driver's timeout logic rather than
 * being re-derived from the rpm value, so a failed sensor is handled the
 * same way in both places. */
void spindle_sm_update(spindle_sm_t *sm, const spindle_sm_cfg_t *cfg,
                       float rpm, bool rpm_stopped,
                       float current_a, float pressure,
                       int64_t now_us);

/* Retrieve the summary for the cut that just ended. Returns false unless
 * cycle_completed is set on this pass. */
bool spindle_sm_take_cycle(spindle_sm_t *sm, cycle_summary_t *out);

const char *spindle_state_str(spindle_state_t s);

#ifdef __cplusplus
}
#endif
