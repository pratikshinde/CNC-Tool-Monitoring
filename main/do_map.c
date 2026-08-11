/*
 * do_map.c
 */

#include "do_map.h"

static bool spindle_alarm(const do_map_input_t *in, unsigned s)
{
    if (s >= NUM_SPINDLES || !in->spindle_enabled[s] || !in->alarm[s]) return false;
    return in->alarm[s]->any_alarm;
}

static bool spindle_warning(const do_map_input_t *in, unsigned s)
{
    if (s >= NUM_SPINDLES || !in->spindle_enabled[s] || !in->alarm[s]) return false;
    return in->alarm[s]->any_warning;
}

/* Is quantity q at fault on spindle s? Same active-or-latched test alarm.c
 * itself uses to roll bands up into any_alarm/any_warning (alarm.c:195-201),
 * applied per quantity instead of across all of them, plus whatever else is
 * specific to that quantity: breakage/crash/wear-trend are current-signature
 * conditions, and a suspect RPM sensor (current flowing, no pulses) is an
 * RPM-side anomaly even though it is not itself a band violation. */
static bool quantity_fault(const do_map_input_t *in, unsigned s, quantity_t q)
{
    if (s >= NUM_SPINDLES || !in->spindle_enabled[s] || !in->alarm[s]) return false;
    const alarm_state_t *a = in->alarm[s];

    for (int b = 0; b < BAND_COUNT; b++) {
        if (a->bands[q][b].active || a->bands[q][b].latched) return true;
    }

    if (q == QTY_CURRENT) {
        if (a->breakage || a->breakage_latched) return true;
        if (a->crash    || a->crash_latched)    return true;
        if (a->trend)                           return true;
    }
    if (q == QTY_RPM && in->rpm_sensor_suspect[s]) return true;

    return false;
}

static bool spindle_quantity(const do_map_input_t *in, const do_cfg_t *dc)
{
    unsigned s = dc->spindle;
    for (int q = 0; q < QTY_COUNT; q++) {
        if ((dc->quantity_mask & (1u << q)) && quantity_fault(in, s, (quantity_t)q)) {
            return true;
        }
    }
    return false;
}

void do_map_evaluate(const app_config_t *cfg, const do_map_input_t *in,
                     bool out[NUM_DIGITAL_OUT])
{
    for (int d = 0; d < NUM_DIGITAL_OUT; d++) {
        const do_cfg_t *dc = &cfg->dout[d];
        bool demand = false;

        switch (dc->source) {

        case DO_SRC_DISABLED:
            demand = false;
            break;

        case DO_SRC_SPINDLE_ALARM:
            demand = spindle_alarm(in, dc->spindle);
            break;

        case DO_SRC_SPINDLE_WARNING:
            demand = spindle_warning(in, dc->spindle);
            break;

        case DO_SRC_ANY_ALARM:
            for (unsigned s = 0; s < NUM_SPINDLES; s++) {
                if (spindle_alarm(in, s)) { demand = true; break; }
            }
            break;

        case DO_SRC_ANY_WARNING:
            for (unsigned s = 0; s < NUM_SPINDLES; s++) {
                if (spindle_warning(in, s)) { demand = true; break; }
            }
            break;

        case DO_SRC_SYSTEM_HEALTHY:
            /* Asserted while healthy. Paired with invert=true in the
             * default config, so the field contact is closed in normal
             * operation and opens on fault, power loss or a hung
             * processor — the three cases a PLC needs to treat alike. */
            demand = in->system_healthy;
            break;

        case DO_SRC_DIAG_FAULT:
            demand = in->diagnostic_fault;
            break;

        case DO_SRC_SPINDLE_QUANTITY:
            if (dc->spindle < NUM_SPINDLES && in->spindle_enabled[dc->spindle]) {
                demand = spindle_quantity(in, dc);
            }
            break;

        default:
            demand = false;
            break;
        }

        out[d] = demand;
    }
}
