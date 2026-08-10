/*
 * rpm.h — spindle speed from pulse inputs, using the ESP32 PCNT hardware.
 *
 * PCNT rather than GPIO interrupts (AI-R8): at 24 000 RPM with a 64 PPR
 * encoder the pulse train is 25.6 kHz, and servicing that in an ISR while
 * the Wi-Fi stack is also asking for CPU is a reliable way to lose counts.
 * The counter is in hardware and cannot miss an edge.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    rpm;
    bool     stopped;         /* no pulses within the configured timeout */
    uint32_t pulses_total;    /* since boot, for diagnostics             */
    bool     used_period_mode;/* which estimator produced this reading   */
} rpm_reading_t;

/* Create one PCNT unit per RPM channel. */
esp_err_t rpm_init(const app_config_t *cfg);

/* Re-apply glitch filter and PPR after a configuration change. */
esp_err_t rpm_reconfigure(const app_config_t *cfg);

/* Sample a channel. Call at a steady interval — the frequency estimator
 * uses the interval between calls as its gate window. */
esp_err_t rpm_read(uint8_t channel, const rpm_cfg_t *cfg, rpm_reading_t *out);

#ifdef __cplusplus
}
#endif
