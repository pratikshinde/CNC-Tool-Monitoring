/*
 * trend.h — short-term history for the live graph.
 *
 * A RAM ring buffer, nothing more. It exists so the Trends page shows a
 * populated graph the instant it loads instead of starting blank and
 * filling in over the next five minutes.
 *
 * Deliberately not the data logger: nothing here reaches flash, and the
 * history does not survive a reboot. Durable, long-range history against
 * the `logs` partition (LG-R1…R7) is a separate job with different
 * requirements — retention, downsampling tiers, wear levelling — and
 * conflating the two would compromise both.
 *
 * Sampling runs on CORE_NETWORK off the published snapshot rather than
 * being fed from the measurement loop, so a slow or blocked reader can
 * never add latency to the alarm path on core 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 300 samples at 1 Hz = 5 minutes. Costs NUM_SPINDLES * 300 * 3 floats,
 * about 7 KB for two spindles — affordable in RAM, and enough context to
 * see a cut start, run and finish. */
#define TREND_CAPACITY   300
#define TREND_PERIOD_MS  1000

typedef struct {
    float current_a;
    float pressure;
    float rpm;
} trend_sample_t;

/* Start the 1 Hz sampler. */
esp_err_t trend_start(void);

/* Copy up to `max` of the most recent samples for one spindle into `out`,
 * oldest first. Returns the number written. `age_ms_out`, if given,
 * receives the age of the newest sample so a client can align the series
 * against its own clock. */
size_t trend_get(uint8_t spindle, trend_sample_t *out, size_t max,
                 uint32_t *age_ms_out);

#ifdef __cplusplus
}
#endif
