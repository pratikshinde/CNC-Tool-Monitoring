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

        default:
            demand = false;
            break;
        }

        out[d] = demand;
    }
}
