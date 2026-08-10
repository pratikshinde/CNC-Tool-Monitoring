/*
 * monitor.c — real-time acquisition, state machine and alarm evaluation.
 *
 * One task, pinned to core 0, running a fixed-period loop:
 *
 *   read RPM  ->  read pressure  ->  read current (RMS burst)
 *      ->  advance state machine  ->  evaluate alarms
 *      ->  map to digital outputs  ->  publish snapshot
 *
 * Alarms are evaluated in this same task rather than a separate one. An
 * earlier design had them split, with a queue in between, on the theory
 * that it was cleaner. It is not: it adds a scheduling hop inside the
 * 200 ms latency budget and buys nothing, because the alarm evaluation is
 * pure arithmetic on data that has just been produced here.
 *
 * The loop period is dominated by the current burst — see the accuracy
 * discussion in analog.c. With the default 128-sample burst on two
 * spindles the real period is around 320 ms, not the 100 ms in the
 * config. The loop measures and publishes its own period so this is
 * visible rather than assumed.
 */

#include "monitor.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "board.h"
#include "config_store.h"
#include "dio.h"
#include "do_map.h"

static const char *TAG = "monitor";

/* 1-second moving average of current (AI-R6). Sized generously; the loop
 * runs slower than this in practice so the window is time-bounded rather
 * than sample-bounded. */
#define AVG_WINDOW 16

typedef struct {
    float    buf[AVG_WINDOW];
    int64_t  t[AVG_WINDOW];
    uint8_t  head;
    uint8_t  count;
} avg_win_t;

typedef struct {
    spindle_sm_t  sm;
    alarm_state_t alarm;
    avg_win_t     avg;
    float         peak_hold;
} spindle_rt_t;

static spindle_rt_t       s_rt[NUM_SPINDLES];
static monitor_snapshot_t s_snap;
static SemaphoreHandle_t  s_snap_mutex;
static TaskHandle_t       s_task;
static volatile bool      s_ack_request[NUM_SPINDLES];
static volatile bool      s_cfg_dirty;

/* ============================================================
 * Moving average
 * ============================================================ */

static void avg_push(avg_win_t *a, float v, int64_t now)
{
    a->buf[a->head] = v;
    a->t[a->head] = now;
    a->head = (uint8_t)((a->head + 1) % AVG_WINDOW);
    if (a->count < AVG_WINDOW) a->count++;
}

static float avg_value(const avg_win_t *a, int64_t now, int64_t window_us)
{
    float sum = 0.0f;
    int n = 0;
    for (int i = 0; i < a->count; i++) {
        int idx = (a->head - 1 - i + AVG_WINDOW * 2) % AVG_WINDOW;
        if (now - a->t[idx] <= window_us) {
            sum += a->buf[idx];
            n++;
        }
    }
    return n ? (sum / (float)n) : 0.0f;
}

/* ============================================================
 * The loop
 * ============================================================ */

static void monitor_task(void *arg)
{
    (void)arg;

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    int64_t now = esp_timer_get_time();
    for (int i = 0; i < NUM_SPINDLES; i++) {
        spindle_sm_init(&s_rt[i].sm, now);
        alarm_init(&s_rt[i].alarm);
    }

    int64_t last_loop_us = now;
    bool first_pass_done = false;

    for (;;) {
        const app_config_t *cfg = config_get();
        int64_t loop_start = esp_timer_get_time();

        bool any_diag_fault = false;
        bool adc_ok = analog_is_healthy();

        for (uint8_t i = 0; i < NUM_SPINDLES; i++) {
            const spindle_cfg_t *sc = &cfg->spindle[i];
            spindle_rt_t *rt = &s_rt[i];

            if (!sc->enabled) continue;

            /* --- Acquire ------------------------------------------- */

            rpm_reading_t rr = {0};
            rpm_read(i, &sc->rpm, &rr);

            pressure_reading_t pr = {0};
            analog_read_pressure(i, &sc->pressure, &pr);

            current_reading_t cr = {0};
            analog_read_current(i, &sc->current, &cr);

            now = esp_timer_get_time();

            /* --- Cross-check: current without rotation (DG-R2) ------
             * Real cutting current with the RPM input reading zero means
             * the speed sensor has failed, not that the spindle is
             * stopped. Treating it as "stopped" would silently disarm all
             * monitoring — the worst possible failure mode, because
             * everything looks normal while nothing is being watched. */
            bool rpm_suspect = rr.stopped &&
                               (cr.amps > sc->sm.cut_detect_current_a) &&
                               (cr.status == SENSOR_OK);

            if (rpm_suspect) {
                any_diag_fault = true;
            }
            if (pr.status != SENSOR_OK || cr.status != SENSOR_OK) {
                any_diag_fault = true;
            }

            /* --- State machine -------------------------------------- */

            spindle_sm_update(&rt->sm, &sc->sm,
                              rr.rpm, rr.stopped && !rpm_suspect,
                              cr.amps, pr.value, now);

            if (rt->sm.state == SPINDLE_CUTTING) {
                if (cr.amps > rt->peak_hold) rt->peak_hold = cr.amps;
            } else {
                rt->peak_hold = 0.0f;
            }

            avg_push(&rt->avg, cr.amps, now);

            /* --- Alarms --------------------------------------------- */

            alarm_input_t ai = {0};
            ai.value[QTY_CURRENT]  = cr.amps;
            ai.value[QTY_PRESSURE] = pr.value;
            ai.value[QTY_RPM]      = rr.rpm;
            ai.quantity_faulted[QTY_CURRENT]  = (cr.status != SENSOR_OK);
            ai.quantity_faulted[QTY_PRESSURE] = (pr.status != SENSOR_OK);
            ai.quantity_faulted[QTY_RPM]      = rpm_suspect;

            alarm_update(&rt->alarm, sc, &ai, rt->sm.monitoring_armed, now);

            /* --- Cycle boundary ------------------------------------- */

            cycle_summary_t cyc;
            if (spindle_sm_take_cycle(&rt->sm, &cyc)) {
                alarm_on_cycle_end(&rt->alarm, &sc->wear, cyc.mean_current);
                ESP_LOGI(TAG,
                         "S%u cycle %lu: %lu ms, mean %.2f A, peak %.2f A, "
                         "mean %.1f %s",
                         i, (unsigned long)rt->sm.cycle_count,
                         (unsigned long)cyc.duration_ms,
                         cyc.mean_current, cyc.peak_current,
                         cyc.mean_pressure, sc->pressure.unit);
                s_snap.spindle[i].last_cycle_mean_a = cyc.mean_current;
            }

            /* --- Acknowledge requests -------------------------------- */

            if (s_ack_request[i]) {
                s_ack_request[i] = false;
                alarm_acknowledge(&rt->alarm);
                ESP_LOGI(TAG, "S%u alarms acknowledged", i);
            }

            /* --- Publish -------------------------------------------- */

            spindle_snapshot_t snap = {
                .state             = rt->sm.state,
                .monitoring_armed  = rt->sm.monitoring_armed,
                .current_a         = cr.amps,
                .current_avg_a     = avg_value(&rt->avg, now, 1000000),
                .current_peak_a    = rt->peak_hold,
                .pressure          = pr.value,
                .rpm               = rr.rpm,
                .burden_vrms        = cr.burden_vrms,
                .pressure_adc_volts = pr.adc_volts,
                .pressure_loop_ma   = pr.loop_ma,
                .current_status    = cr.status,
                .pressure_status   = pr.status,
                .rpm_sensor_suspect= rpm_suspect,
                .severity          = rt->alarm.severity,
                .any_alarm         = rt->alarm.any_alarm,
                .any_warning       = rt->alarm.any_warning,
                .cycle_count       = rt->sm.cycle_count,
                .last_cycle_ms     = rt->sm.last_cycle_ms,
                .last_cycle_mean_a = s_snap.spindle[i].last_cycle_mean_a,
            };

            xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
            s_snap.spindle[i] = snap;
            xSemaphoreGive(s_snap_mutex);
        }

        /* --- Digital outputs ---------------------------------------- */

        bool healthy = adc_ok && !any_diag_fault;

        do_map_input_t dmi = {
            .system_healthy   = healthy,
            .diagnostic_fault = any_diag_fault,
        };
        for (int i = 0; i < NUM_SPINDLES; i++) {
            dmi.alarm[i] = &s_rt[i].alarm;
            dmi.spindle_enabled[i] = cfg->spindle[i].enabled;
        }

        bool demand[NUM_DIGITAL_OUT];
        do_map_evaluate(cfg, &dmi, demand);

        for (int d = 0; d < NUM_DIGITAL_OUT; d++) {
            dio_set((uint8_t)d, demand[d]);
        }
        dio_service();

        /* Outputs stay in their safe state until one complete cycle has
         * run, so a garbage first reading can never pulse the PLC
         * (DO-R5). */
        if (!first_pass_done) {
            first_pass_done = true;
            dio_enable_outputs();
            ESP_LOGI(TAG, "first measurement cycle complete");
        }

        if (s_cfg_dirty) {
            s_cfg_dirty = false;
            rpm_reconfigure(cfg);
            ESP_LOGI(TAG, "configuration reloaded");
        }

        /* --- Timing bookkeeping -------------------------------------- */

        int64_t loop_end = esp_timer_get_time();
        uint32_t elapsed_ms = (uint32_t)((loop_end - last_loop_us) / 1000);
        last_loop_us = loop_end;

        xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
        s_snap.adc_healthy      = adc_ok;
        s_snap.system_healthy   = healthy;
        s_snap.diagnostic_fault = any_diag_fault;
        s_snap.loop_period_ms   = elapsed_ms;
        if (elapsed_ms > s_snap.worst_loop_ms) s_snap.worst_loop_ms = elapsed_ms;
        s_snap.uptime_us        = loop_end;
        xSemaphoreGive(s_snap_mutex);

        esp_task_wdt_reset();

        /* Sleep only for whatever is left of the configured period. When
         * the burst has already overrun it, yield one tick so lower
         * priority work on this core is not starved, and carry on. */
        uint32_t work_ms = (uint32_t)((loop_end - loop_start) / 1000);
        uint32_t target  = cfg->system.measure_period_ms;
        vTaskDelay(pdMS_TO_TICKS(work_ms < target ? (target - work_ms) : 1));
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t monitor_start(void)
{
    s_snap_mutex = xSemaphoreCreateMutex();
    if (!s_snap_mutex) return ESP_ERR_NO_MEM;

    memset(&s_snap, 0, sizeof(s_snap));
    memset(s_rt, 0, sizeof(s_rt));

    BaseType_t ok = xTaskCreatePinnedToCore(monitor_task, "monitor",
                                            STACK_MEASURE, NULL,
                                            PRIO_ALARM, &s_task,
                                            CORE_REALTIME);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

void monitor_get_snapshot(monitor_snapshot_t *out)
{
    xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
    memcpy(out, &s_snap, sizeof(*out));
    xSemaphoreGive(s_snap_mutex);
}

void monitor_acknowledge(uint8_t spindle)
{
    if (spindle >= NUM_SPINDLES) {
        for (int i = 0; i < NUM_SPINDLES; i++) s_ack_request[i] = true;
    } else {
        s_ack_request[spindle] = true;
    }
}

void monitor_config_changed(void)
{
    s_cfg_dirty = true;
}
