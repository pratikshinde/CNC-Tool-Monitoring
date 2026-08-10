/*
 * spindle_sm.c
 */

#include "spindle_sm.h"

#include <string.h>

static void enter(spindle_sm_t *sm, spindle_state_t next, int64_t now_us)
{
    if (sm->state == next) return;

    sm->state = next;
    sm->state_since_us = now_us;

    if (next == SPINDLE_CUTTING) {
        sm->cut_started_us    = now_us;
        sm->cut_current_sum   = 0.0f;
        sm->cut_current_peak  = 0.0f;
        sm->cut_pressure_sum  = 0.0f;
        sm->cut_samples       = 0;
        /* Disarmed until the inhibit window expires — the tool entering
         * material produces a current spike that is entirely normal. */
        sm->monitoring_armed  = false;
    } else {
        sm->monitoring_armed  = false;
    }
}

void spindle_sm_init(spindle_sm_t *sm, int64_t now_us)
{
    memset(sm, 0, sizeof(*sm));
    sm->state = SPINDLE_STOPPED;
    sm->state_since_us = now_us;
}

void spindle_sm_update(spindle_sm_t *sm, const spindle_sm_cfg_t *cfg,
                       float rpm, bool rpm_stopped,
                       float current_a, float pressure,
                       int64_t now_us)
{
    sm->cycle_completed = false;

    const bool turning = !rpm_stopped && (rpm >= cfg->start_rpm);
    const int64_t in_state_ms = (now_us - sm->state_since_us) / 1000;

    switch (sm->state) {

    case SPINDLE_STOPPED:
        if (turning) enter(sm, SPINDLE_SPIN_UP, now_us);
        break;

    case SPINDLE_SPIN_UP:
        if (!turning) {
            enter(sm, SPINDLE_COAST_DOWN, now_us);
        } else if (in_state_ms >= (int64_t)cfg->settle_ms) {
            /* Speed has been above the start threshold for the settle
             * period. Note this does not check that RPM is *stable*, only
             * that it has been present — a spindle ramping to a
             * programmed speed is still legitimately spinning up, and
             * demanding stability here would stall the machine in
             * SPIN_UP during long ramps on large tools. */
            enter(sm, SPINDLE_IDLE, now_us);
        }
        break;

    case SPINDLE_IDLE:
        if (!turning) {
            enter(sm, SPINDLE_COAST_DOWN, now_us);
        } else if (current_a >= cfg->cut_detect_current_a) {
            enter(sm, SPINDLE_CUTTING, now_us);
        }
        break;

    case SPINDLE_CUTTING:
        if (!turning) {
            /* Spindle stopped mid-cut. Close the cycle out — the data up
             * to this point is still the record of what happened, and
             * discarding it would lose the evidence of whatever caused
             * the stop. */
            sm->last_cycle_ms = (uint32_t)((now_us - sm->cut_started_us) / 1000);
            sm->cycle_completed = true;
            sm->cycle_count++;
            enter(sm, SPINDLE_COAST_DOWN, now_us);
            break;
        }

        if (current_a < cfg->idle_current_a) {
            sm->last_cycle_ms = (uint32_t)((now_us - sm->cut_started_us) / 1000);
            sm->cycle_completed = true;
            sm->cycle_count++;
            enter(sm, SPINDLE_IDLE, now_us);
            break;
        }

        /* Accumulate for the cycle summary. */
        sm->cut_current_sum  += current_a;
        sm->cut_pressure_sum += pressure;
        sm->cut_samples++;
        if (current_a > sm->cut_current_peak) sm->cut_current_peak = current_a;

        if (!sm->monitoring_armed &&
            in_state_ms >= (int64_t)cfg->alarm_inhibit_ms) {
            sm->monitoring_armed = true;
        }
        break;

    case SPINDLE_COAST_DOWN:
        if (turning) {
            /* Picked up again before coming to rest. */
            enter(sm, SPINDLE_IDLE, now_us);
        } else if (rpm_stopped && current_a < cfg->idle_current_a) {
            enter(sm, SPINDLE_STOPPED, now_us);
        }
        break;

    default:
        enter(sm, SPINDLE_STOPPED, now_us);
        break;
    }
}

bool spindle_sm_take_cycle(spindle_sm_t *sm, cycle_summary_t *out)
{
    if (!sm->cycle_completed || !out) return false;

    uint32_t n = sm->cut_samples;

    out->duration_ms   = sm->last_cycle_ms;
    out->samples       = n;
    out->peak_current  = sm->cut_current_peak;
    out->mean_current  = n ? (sm->cut_current_sum  / (float)n) : 0.0f;
    out->mean_pressure = n ? (sm->cut_pressure_sum / (float)n) : 0.0f;

    return true;
}

const char *spindle_state_str(spindle_state_t s)
{
    switch (s) {
    case SPINDLE_STOPPED:     return "STOPPED";
    case SPINDLE_SPIN_UP:     return "SPIN_UP";
    case SPINDLE_IDLE:        return "IDLE";
    case SPINDLE_CUTTING:     return "CUTTING";
    case SPINDLE_COAST_DOWN:  return "COAST_DOWN";
    default:                  return "?";
    }
}
