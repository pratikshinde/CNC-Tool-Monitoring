/*
 * calib.h — guided field calibration for the current and pressure inputs.
 *
 * The problem this solves: correcting a reading meant editing
 * gain_correction in C and reflashing. Here the operator applies a known
 * value, types what their reference instrument reads, and the device
 * solves for the correction itself.
 *
 * Pressure uses a two-point fit, and it has to. A burden-resistor
 * tolerance error is a gain error on the *loop current*, but the 4 mA live
 * zero means that arrives in engineering units as an affine error — a
 * slope and an intercept. One point can only solve for one of them, so a
 * single-gain tweak that makes 30 bar correct will leave 200 bar wrong.
 * Two points solve both exactly.
 *
 * Current is different: no primary current means no secondary voltage, so
 * the relationship genuinely passes through the origin and one point at a
 * known load is sufficient.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CALIB_CURRENT  = 0,
    CALIB_PRESSURE = 1,
    CALIB_TARGET_COUNT
} calib_target_t;

#define CALIB_MAX_POINTS 2

typedef struct {
    bool  captured;
    float nominal;    /* uncorrected engineering value at capture time */
    float reference;  /* what the operator's instrument read           */
} calib_point_t;

typedef struct {
    calib_point_t point[CALIB_MAX_POINTS];
    float         live_nominal;   /* uncorrected value right now */
    float         live_corrected; /* what the device reports right now */
    float         raw;            /* burden V (current) or loop mA (pressure) */
} calib_state_t;

/* Capture one calibration point: pairs the live uncorrected reading with
 * the reference value the operator supplies. `index` is 0 or 1; current
 * only uses index 0. */
esp_err_t calib_capture(uint8_t spindle, calib_target_t target,
                        uint8_t index, float reference);

/* Solve for gain/offset from the captured points and persist through
 * config_store_commit(). Returns ESP_ERR_INVALID_STATE if not enough
 * points are captured, or if two points are too close together to define
 * a slope. On a validation failure *reason carries the detail. */
esp_err_t calib_apply(uint8_t spindle, calib_target_t target,
                      cfg_result_t *reason);

/* Forget captured points without touching the stored calibration. */
void calib_reset(uint8_t spindle, calib_target_t target);

/* Captured points plus live readings, for the calibration screen. */
void calib_get_state(uint8_t spindle, calib_target_t target,
                     calib_state_t *out);

/* Re-measure the CT channel's DC bias and persist it as the new zero
 * reference. The spindle must actually be stopped — this cannot tell a
 * stopped spindle from a broken CT, so the UI is responsible for saying so.
 */
esp_err_t calib_autozero(uint8_t spindle);

#ifdef __cplusplus
}
#endif
