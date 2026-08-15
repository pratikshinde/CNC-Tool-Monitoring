/*
 * scaling.h — raw volts to engineering units.
 *
 * Pure C and free of ESP-IDF so the conversion maths can be exercised on a
 * host. Getting a scaling factor wrong is silent: the number still looks
 * plausible, it is just wrong, and it will be wrong for the entire life of
 * the installation. These functions are worth testing directly.
 */
#pragma once

#include <stdbool.h>

#include "spindle_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * SMU analogue front-end electrical constants
 *
 * These describe the SMU's own hardware (SMU_HARDWARE_REQUIREMENTS.md
 * §3.2), not the ESP32's — moved here rather than left in board.h because
 * the ADC-to-engineering-units maths that consumes them now runs on the
 * SMU. board.h remains the ESP32's own pin/electrical map and has no
 * reason to know these values in V2.
 *
 * PRESSURE_BURDEN_OHM is 100, NOT the 180 that V1's board.h carried. That
 * 180R was a known, documented defect: at 20 mA full-scale it produces
 * 3.6 V, which exceeds the 3.3 V ADC rail and defeats the over-range fault
 * check (a shorted transmitter driving max loop current would not read as
 * a fault). Carrying the old constant forward here would silently
 * reintroduce a defect the hardware document already fixed for the SMU
 * redesign — see SMU_HARDWARE_REQUIREMENTS.md §3.2.
 * ============================================================ */

#define PRESSURE_BURDEN_OHM         100.0f
#define PRESSURE_DIV_R_TOP_OHM      80600.0f
#define PRESSURE_DIV_R_BOT_OHM      20000.0f
#define PRESSURE_DIV_RATIO \
    (PRESSURE_DIV_R_BOT_OHM / (PRESSURE_DIV_R_TOP_OHM + PRESSURE_DIV_R_BOT_OHM))

#define CT_BURDEN_OHM               0.1f
#define CT_BIAS_VOLTS               1.65f    /* nominal; measured at auto-zero */

/* Sensor health, evaluated from the raw signal before scaling (AI-R3). */
typedef enum {
    SENSOR_OK = 0,
    SENSOR_OPEN,        /* < 3.5 mA: broken wire or dead transmitter */
    SENSOR_OVERRANGE,   /* > 21 mA, or above the divider's ceiling   */
    SENSOR_UNDERRANGE,  /* below the 0 V floor by more than noise    */
} sensor_status_t;

/* Convert the voltage at the ADC pin into engineering units.
 * Returns the sensor status; *out is only meaningful when SENSOR_OK.
 *
 * The status is deliberately separate from the value. A faulted channel
 * must raise a diagnostic, never a process alarm, and collapsing the two
 * into a magic value would make that distinction easy to lose. */
sensor_status_t pressure_scale(const pressure_cfg_t *cfg, float adc_volts,
                               float *out);

/* Convert an RMS voltage across the CT burden into primary amperes. */
float current_scale(const current_cfg_t *cfg, float burden_vrms);

/* Loop current in milliamps, for diagnostics display. */
float pressure_loop_ma(float adc_volts);

/* Map an RPM pulse interval to revolutions per minute.
 * interval_us is the mean time between consecutive pulses. */
float rpm_from_interval(float interval_us, unsigned pulses_per_rev);

/* Map a pulse count over a gate window to revolutions per minute. */
float rpm_from_count(unsigned pulses, float gate_ms, unsigned pulses_per_rev);

#ifdef __cplusplus
}
#endif
