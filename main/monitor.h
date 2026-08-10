/*
 * monitor.h — the real-time measurement and alarm task.
 *
 * Owns core 0. Everything that has to happen inside the NFR-1 latency
 * budget lives here; nothing in this file may block on the network.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "alarm.h"
#include "analog.h"
#include "app_config.h"
#include "rpm.h"
#include "spindle_sm.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Published snapshot of one spindle, for the UI and the logger.
 * Copied out under a mutex; never referenced directly by other tasks. */
typedef struct {
    spindle_state_t state;
    bool            monitoring_armed;

    float           current_a;
    float           current_avg_a;   /* 1 s moving average (AI-R6) */
    float           current_peak_a;  /* peak hold this cut          */
    float           pressure;
    float           rpm;

    /* Pre-scaling values, republished so the calibration screen can show
     * what it is actually working from and so calib.c can recompute an
     * uncorrected engineering value without going back to the ADC and
     * contending with the measurement loop for the bus. */
    float           burden_vrms;
    float           pressure_adc_volts;
    float           pressure_loop_ma;

    sensor_status_t current_status;
    sensor_status_t pressure_status;
    bool            rpm_sensor_suspect;  /* current flowing, no pulses */

    severity_t      severity;
    bool            any_alarm;
    bool            any_warning;

    uint32_t        cycle_count;
    uint32_t        last_cycle_ms;
    float           last_cycle_mean_a;
} spindle_snapshot_t;

typedef struct {
    spindle_snapshot_t spindle[NUM_SPINDLES];
    bool               adc_healthy;
    bool               system_healthy;
    bool               diagnostic_fault;
    uint32_t           loop_period_ms;   /* measured, not configured */
    uint32_t           worst_loop_ms;
    int64_t            uptime_us;
} monitor_snapshot_t;

esp_err_t monitor_start(void);

/* Thread-safe copy of the latest published state. */
void monitor_get_snapshot(monitor_snapshot_t *out);

/* Acknowledge latched alarms on one spindle, or all if spindle >= count. */
void monitor_acknowledge(uint8_t spindle);

/* Re-read configuration after a commit. Cheap and non-blocking: the task
 * picks up the new pointer contents on its next pass. */
void monitor_config_changed(void);

#ifdef __cplusplus
}
#endif
