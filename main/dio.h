/*
 * dio.h — digital inputs and PLC-facing digital outputs.
 *
 * The output side owns the safe-state contract (DO-R5): outputs are
 * inactive from reset until the application explicitly releases them, and
 * a de-init or a panic must leave them inactive rather than floating.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure pins. All outputs are driven to their inactive state before
 * the pin direction is set to output, so the first electrical transition
 * the PLC sees is already the safe one. */
esp_err_t dio_init(const app_config_t *cfg);

/* Outputs stay forced inactive until this is called. main() releases them
 * only after the first complete measurement cycle. */
void dio_enable_outputs(void);

/* Request an output state. `active` is the logical sense — the inversion
 * configured for fail-safe operation is applied inside.
 *
 * Minimum pulse width (DO-R4) is enforced here: a request to clear an
 * output that has not yet been active for min_pulse_ms is deferred, and
 * applied by dio_service(). */
void dio_set(uint8_t index, bool active);

/* Drive the deferred pulse-width logic. Call every measurement cycle. */
void dio_service(void);

/* Present logical state of an output. */
bool dio_get(uint8_t index);

/* Read a digital input, already corrected for the opto inversion:
 * true means the field input is energised. */
bool dio_read_input(uint8_t index);

/* Force an output for the diagnostics self-test (DG-R4). While any output
 * is forced, dio_set() requests for that output are ignored, so a test
 * cannot be silently overridden by the alarm engine. */
void dio_force(uint8_t index, bool active, bool forced);
bool dio_is_forced(uint8_t index);
void dio_clear_all_forces(void);

#ifdef __cplusplus
}
#endif
