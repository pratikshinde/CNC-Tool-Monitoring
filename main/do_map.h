/*
 * do_map.h — resolve alarm state into digital output demands (DO-R1).
 *
 * Pure function: given the configuration and the current alarm state of
 * every spindle, produce the logical state each output should be in.
 * Deliberately separated from dio.c so the mapping can be tested without
 * any GPIO, and so the electrical concerns (inversion, pulse stretching)
 * stay in one place and the logical ones in another.
 */
#pragma once

#include <stdbool.h>

#include "alarm.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const alarm_state_t *alarm[NUM_SPINDLES];
    bool spindle_enabled[NUM_SPINDLES];

    /* Current flowing with no RPM pulses — a broken speed sensor, not a
     * stopped spindle (see monitor.c). Folded into DO_SRC_SPINDLE_QUANTITY's
     * RPM bit alongside the RPM bands themselves, since a dead sensor is
     * exactly the kind of "RPM anomaly" a PLC-facing output should catch. */
    bool rpm_sensor_suspect[NUM_SPINDLES];

    /* System-level inputs that are not per-spindle. */
    bool system_healthy;   /* all tasks alive, ADC responding, config valid */
    bool diagnostic_fault; /* any sensor or hardware fault                  */
} do_map_input_t;

/* Fill out[] with the logical (pre-inversion) demand for each output. */
void do_map_evaluate(const app_config_t *cfg, const do_map_input_t *in,
                     bool out[NUM_DIGITAL_OUT]);

#ifdef __cplusplus
}
#endif
