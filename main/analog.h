/*
 * analog.h — current and pressure acquisition.
 *
 * The current path sits behind a small interface (current_source_t) so the
 * measurement chain can be swapped without touching anything above it.
 * This matters: the ADS1115-direct implementation shipped here has a known
 * accuracy ceiling (see analog.c), and replacing it with an external
 * RMS-to-DC front end or ESP32 I2S sampling should be a one-line change at
 * the call site, not a rewrite.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "scaling.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float           amps;
    float           burden_vrms;   /* pre-scaling, for diagnostics */
    sensor_status_t status;
    uint32_t        window_us;     /* how long the burst actually took */
} current_reading_t;

typedef struct {
    float           value;         /* engineering units */
    float           adc_volts;     /* raw, for diagnostics */
    float           loop_ma;       /* 4-20 mA mode only  */
    sensor_status_t status;
} pressure_reading_t;

/* Bring up the I2C bus and the ADS1115, seeding each channel's zero
 * reference from the stored calibration so a previous auto-zero survives a
 * reboot. Fails if the ADC does not respond, because everything downstream
 * is meaningless without it. */
esp_err_t analog_init(const app_config_t *cfg);

/* True once the ADC has been probed successfully. The diagnostics layer
 * polls this to distinguish "no data yet" from "hardware missing". */
bool analog_is_healthy(void);

/* Acquire one RMS burst on a spindle's current channel. Blocking; takes
 * roughly (rms_burst_samples / 860) seconds. */
esp_err_t analog_read_current(uint8_t spindle, const current_cfg_t *cfg,
                              current_reading_t *out);

/* Single-shot read of a spindle's pressure channel. */
esp_err_t analog_read_pressure(uint8_t spindle, const pressure_cfg_t *cfg,
                               pressure_reading_t *out);

/* Measure the CT channel's DC bias with the spindle confirmed stopped and
 * store it as the new zero reference (AI-R7). The caller is responsible
 * for confirming the machine really is stopped — this function cannot
 * tell the difference between a stopped spindle and a broken CT. */
esp_err_t analog_autozero(uint8_t spindle, float *measured_bias_v);

/* Raw ADC counts for the diagnostics page (DG-R1). */
esp_err_t analog_read_raw(uint8_t ads_channel, int16_t *raw);

#ifdef __cplusplus
}
#endif
