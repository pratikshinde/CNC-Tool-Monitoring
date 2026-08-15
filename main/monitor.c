/*
 * monitor.c — SMU telemetry aggregator (V2 migration Phase 3).
 *
 * Owns two smu_link_t instances (one per spindle) and one transport per
 * link — smu_mock.c's software SMU by default (Kconfig
 * CONFIG_SMU_USE_MOCK), or smu_i2c_transport.c's real I2C driver once
 * hardware exists. Neither smu_link.c's protocol logic nor this file's
 * aggregation logic differs between the two; only which transport gets
 * plugged in changes.
 *
 * Naming note for anyone comparing this against FIRMWARE_DESIGN_SPEC.md
 * or the V2 migration plan: both describe this replacement as a new
 * telemetry.[ch] with renamed functions (monitor_get_snapshot ->
 * telemetry_get_snapshot, etc). This file keeps monitor.[ch]'s existing
 * names instead — a deliberate deviation made while implementing this
 * phase, not an oversight. The rename would have touched web.c, modbus.c,
 * calib.c, trend.c and main.c for a purely cosmetic gain: "monitor" is
 * still an accurate name for "the subsystem that reports spindle state",
 * and every one of those files already compiles against this exact API.
 * Keeping the name minimises the blast radius of a phase that already
 * touches a lot of ground.
 *
 * Three tasks: one poller per SMU link at 20 Hz (FIRMWARE_DESIGN_SPEC.md
 * §3.1/§5.1's report-budget math), plus one aggregator that derives
 * system-level health from both links' freshness and handles config
 * pushes. Splitting config-push out of the pollers matters: a slow
 * two-phase commit (smu_link_push_config's SMU_CONFIG_COMMIT_WAIT_US
 * wait) must never be able to delay the 20 Hz telemetry path.
 */

#include "monitor.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "board.h"
#include "config_store.h"
#include "smu_link.h"

#if CONFIG_SMU_USE_MOCK
#include "smu_mock.h"
#else
#include "smu_i2c_transport.h"
#endif

static const char *TAG = "monitor";

/* 20 Hz, matching the master's poll rate in FIRMWARE_DESIGN_SPEC.md
 * §3.1: 50 ms SMU detection + 50 ms worst-case poll latency = the 100 ms
 * report budget. */
#define SMU_POLL_PERIOD_MS         50

/* A link is "fresh" if its last good frame is no older than this. Four
 * poll periods of grace — enough to absorb one or two genuinely missed
 * polls without flapping system_healthy on every minor hiccup, short
 * enough that "stale" still means something. */
#define SMU_FRESH_MAX_AGE_US       (4 * SMU_POLL_PERIOD_MS * 1000)

/* How long smu_link_push_config() may wait for a commit to land. The
 * mock resolves on its first poll; a real SMU's Data Flash write is
 * expected to be single-digit milliseconds (M2003 page-program figures),
 * so this is generous headroom, not a tuned value. */
#define SMU_CONFIG_COMMIT_WAIT_US  (2 * 1000 * 1000)

typedef struct {
    smu_link_t          link;
#if CONFIG_SMU_USE_MOCK
    smu_mock_spindle_t  mock;
#else
    smu_i2c_ctx_t        i2c_ctx;
#endif
    bool                 transport_ready;

    /* Guards every access to `link` (and, transitively, its transport).
     * smu_task's 20 Hz poll and an out-of-band monitor_smu_command_wait()
     * call (e.g. calib.c's auto-zero, from an HTTP handler on
     * CORE_NETWORK) both reach the same smu_link_t and the same
     * underlying I2C device from different tasks. Without this, the two
     * can interleave transport calls to the same bus, which is a data
     * race at best and bus corruption at worst — the same class of bug
     * modbus.c's s_handle_mutex exists to prevent for its RTU/TCP
     * handles. */
    SemaphoreHandle_t    mutex;
} smu_channel_t;

static smu_channel_t      s_ch[NUM_SPINDLES];
static monitor_snapshot_t s_snap;
static SemaphoreHandle_t  s_snap_mutex;
static volatile bool      s_ack_request[NUM_SPINDLES];
static volatile bool      s_cfg_dirty;

/* ============================================================
 * Wire telemetry -> the existing spindle_snapshot_t shape
 * ============================================================ */

static void translate(const smu_telemetry_t *w, bool fresh, spindle_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));

    out->state            = (spindle_state_t)w->state;
    out->current_a         = w->current_a;
    out->current_avg_a     = w->current_avg_a;
    out->current_peak_a    = w->current_peak_a;
    out->pressure           = w->pressure;
    out->rpm                 = w->rpm;

    out->burden_vrms         = w->ct_burden_vrms;
    out->pressure_adc_volts  = w->pressure_adc_v;
    /* Unconditional on purpose — see scaling.c: only meaningful in
     * 4-20 mA mode, but costs nothing to compute in 0-10 V mode and the
     * caller (the calibration screen) already knows which mode applies. */
    out->pressure_loop_ma    = pressure_loop_ma(w->pressure_adc_v);

    out->current_status      = (sensor_status_t)w->current_sensor;
    out->pressure_status     = (sensor_status_t)w->pressure_sensor;
    out->rpm_sensor_suspect  = (w->diag_flags & SMU_DIAG_RPM_SUSPECT) != 0;

    out->severity             = (severity_t)w->severity;
    out->any_alarm             = (w->status_flags & SMU_STATUS_ANY_ALARM) != 0;
    out->any_warning           = (w->status_flags & SMU_STATUS_ANY_WARNING) != 0;
    out->any_latched           = (w->status_flags & SMU_STATUS_ANY_LATCHED) != 0;
    out->breakage               = (w->status_flags & SMU_STATUS_BREAKAGE) != 0;
    out->crash                   = (w->status_flags & SMU_STATUS_CRASH) != 0;
    out->trend                   = (w->status_flags & SMU_STATUS_TREND) != 0;

    memcpy(out->bands_active,  w->bands_active,  sizeof(out->bands_active));
    memcpy(out->bands_latched, w->bands_latched, sizeof(out->bands_latched));
    out->output_state = w->output_state;
    out->diag_flags    = w->diag_flags;

    out->cycle_count           = w->cycle_count;
    out->last_cycle_ms         = w->last_cycle_ms;
    out->last_cycle_mean_a     = w->last_cycle_mean_a;
    out->last_cycle_peak_a     = w->last_cycle_peak_a;

    /* monitoring_armed is the one field a stale link must force rather
     * than merely echo: an operator or the web UI reading "armed" off a
     * frame that might be minutes old must not be told monitoring is
     * live when the master genuinely cannot see whether it still is.
     * Everything else above is last-known-good by construction (it came
     * from ch->link.last_good, untouched whether fresh or not) — see
     * FIRMWARE_DESIGN_SPEC.md §5.1: stale data is held, never zeroed,
     * and this is the one place "held" is not quite enough on its own. */
    out->monitoring_armed = fresh && ((w->status_flags & SMU_STATUS_ARMED) != 0);
}

static void build_link_status(const smu_link_t *link, smu_link_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state             = link->state;
    out->have_ever_linked  = link->have_good;
    out->crc_error_count   = link->crc_error_count;
    out->timeout_count     = link->timeout_count;
    out->poll_count        = link->poll_count;

    if (link->have_good) {
        int64_t age_us = esp_timer_get_time() - link->last_good_us;
        out->age_ms = (age_us > 0) ? (uint32_t)(age_us / 1000) : 0;
    }
    if (link->ident_valid) {
        out->fw_version         = link->ident.fw_version;
        out->reset_cause        = link->ident.reset_cause;
        out->reset_count        = link->ident.reset_count;
        out->smu_uptime_ms      = link->ident.uptime_ms;
        out->cfg_schema_version = link->ident.cfg_schema_version;
    }
}

/* ============================================================
 * Per-SMU poller
 * ============================================================ */

static void smu_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    smu_channel_t *ch = &s_ch[idx];

    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        if (ch->transport_ready) {
            xSemaphoreTake(ch->mutex, portMAX_DELAY);

            int64_t now_us = esp_timer_get_time();
            smu_link_poll(&ch->link, now_us);

            bool fresh = smu_link_is_fresh(&ch->link, now_us, SMU_FRESH_MAX_AGE_US);
            spindle_snapshot_t snap;
            if (ch->link.have_good) {
                translate(&ch->link.last_good, fresh, &snap);
            } else {
                /* Never had a good frame at all — genuinely nothing to
                 * show, not even stale data. All-zero, and
                 * monitoring_armed is already false from the memset. */
                memset(&snap, 0, sizeof(snap));
            }

            if (s_ack_request[idx]) {
                s_ack_request[idx] = false;
                esp_err_t err = smu_link_send_command(&ch->link, SMU_CMD_ACKNOWLEDGE, 0, 0);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "S%d acknowledge send failed: %s", idx, esp_err_to_name(err));
                }
            }

            smu_link_status_t link_status;
            build_link_status(&ch->link, &link_status);

            xSemaphoreGive(ch->mutex);

            xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
            s_snap.spindle[idx] = snap;
            s_snap.link[idx]    = link_status;
            xSemaphoreGive(s_snap_mutex);
        }

        esp_task_wdt_reset();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SMU_POLL_PERIOD_MS));
    }
}

/* ============================================================
 * System-level aggregation + config push
 * ============================================================ */

static void aggregate_task(void *arg)
{
    (void)arg;
    int64_t last_loop_us = esp_timer_get_time();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SMU_POLL_PERIOD_MS));

        bool any_stale = false;
        bool any_diag  = false;
        for (int i = 0; i < NUM_SPINDLES; i++) {
            bool fresh = s_ch[i].transport_ready &&
                        smu_link_is_fresh(&s_ch[i].link, esp_timer_get_time(),
                                          SMU_FRESH_MAX_AGE_US);
            if (!fresh) any_stale = true;

            xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
            const spindle_snapshot_t *sp = &s_snap.spindle[i];
            if (sp->current_status != SENSOR_OK || sp->pressure_status != SENSOR_OK ||
                sp->rpm_sensor_suspect) {
                any_diag = true;
            }
            xSemaphoreGive(s_snap_mutex);
        }

        int64_t loop_end = esp_timer_get_time();
        uint32_t elapsed_ms = (uint32_t)((loop_end - last_loop_us) / 1000);
        last_loop_us = loop_end;

        xSemaphoreTake(s_snap_mutex, portMAX_DELAY);
        /* adc_healthy is repurposed from V1's "is the local ADC
         * responding" to V2's "are both SMU links fresh" — same
         * underlying question (can this device trust what it is about
         * to report), different mechanism behind the answer. */
        s_snap.adc_healthy      = !any_stale;
        s_snap.diagnostic_fault = any_diag;
        s_snap.system_healthy   = !any_stale && !any_diag;
        s_snap.loop_period_ms   = elapsed_ms;
        if (elapsed_ms > s_snap.worst_loop_ms) s_snap.worst_loop_ms = elapsed_ms;
        s_snap.uptime_us = loop_end;
        xSemaphoreGive(s_snap_mutex);

        if (s_cfg_dirty) {
            s_cfg_dirty = false;
            const app_config_t *cfg = config_get();
            for (int i = 0; i < NUM_SPINDLES; i++) {
                if (!s_ch[i].transport_ready) continue;

                uint8_t result = 0xFF;
                xSemaphoreTake(s_ch[i].mutex, portMAX_DELAY);
                esp_err_t err = smu_link_push_config(
                    &s_ch[i].link, &cfg->spindle[i], CONFIG_SCHEMA_VERSION,
                    cfg->system.mains_hz, SMU_CONFIG_COMMIT_WAIT_US, &result);
                xSemaphoreGive(s_ch[i].mutex);

                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "S%d config push failed: %s", i, esp_err_to_name(err));
                } else if (result != SMU_RESULT_OK) {
                    ESP_LOGW(TAG, "S%d config rejected by SMU (result=%u)", i, result);
                } else {
                    ESP_LOGI(TAG, "S%d config committed", i);
                }
                /* Between spindles, not just once per iteration: two
                 * back-to-back 2s waits would approach
                 * CONFIG_ESP_TASK_WDT_TIMEOUT_S=5 (sdkconfig.defaults)
                 * before this task got back around to resetting it. */
                esp_task_wdt_reset();
            }
        }
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
    memset(s_ch, 0, sizeof(s_ch));

    for (int i = 0; i < NUM_SPINDLES; i++) {
        s_ch[i].mutex = xSemaphoreCreateMutex();
        if (!s_ch[i].mutex) return ESP_ERR_NO_MEM;

        /* Zeroed, not just declared: smu_i2c_transport_init() returns
         * early on failure without touching `t` at all, and this is
         * still passed into smu_link_init() below regardless (so the
         * channel's state stays well-defined even when transport_ready
         * ends up false). A NULL transport.read/write is a clean,
         * debuggable failure if the transport_ready guard is ever
         * bypassed; uninitialised stack garbage would not be. */
        smu_transport_t t = {0};

#if CONFIG_SMU_USE_MOCK
        /* Only the mock needs a clock to seed its synthetic signal's
         * phase origin — declared here, not once for the whole
         * function, so a CONFIG_SMU_USE_MOCK=n build has no unused
         * variable in the branch that never calls smu_mock_init(). */
        int64_t now_us = esp_timer_get_time();
        t = smu_mock_init(&s_ch[i].mock, (uint8_t)i, now_us);
        s_ch[i].transport_ready = true;
#else
        int         port = (i == 0) ? SMU1_I2C_PORT : SMU2_I2C_PORT;
        gpio_num_t  sda  = (i == 0) ? SMU1_I2C_SDA  : SMU2_I2C_SDA;
        gpio_num_t  scl  = (i == 0) ? SMU1_I2C_SCL  : SMU2_I2C_SCL;
        uint16_t    addr = (i == 0) ? SMU_I2C_ADDR_SPINDLE_1 : SMU_I2C_ADDR_SPINDLE_2;

        esp_err_t err = smu_i2c_transport_init(&s_ch[i].i2c_ctx, port, sda, scl, addr, &t);
        if (err != ESP_OK) {
            /* Not fatal to the whole device — see the same reasoning V1
             * applied to a missing ADS1115: come up, report exactly what
             * is wrong, and let the other spindle's link keep working.
             * This channel's link stays permanently DOWN (smu_link_init
             * still runs below so its state is well-defined, just never
             * polled) and its snapshot stays all-zero forever. */
            ESP_LOGE(TAG, "S%d I2C transport init failed: %s", i, esp_err_to_name(err));
            s_ch[i].transport_ready = false;
        } else {
            s_ch[i].transport_ready = true;
        }
#endif
        smu_link_init(&s_ch[i].link, &t, (uint8_t)i);

        char name[8];
        name[0] = 's'; name[1] = 'm'; name[2] = 'u'; name[3] = (char)('0' + i);
        name[4] = '\0';
        BaseType_t ok = xTaskCreatePinnedToCore(
            smu_task, name, STACK_MEASURE, (void *)(intptr_t)i,
            PRIO_ALARM, NULL, CORE_REALTIME);
        if (ok != pdPASS) return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        aggregate_task, "telemetry_agg", STACK_ALARM, NULL,
        PRIO_MEASURE, NULL, CORE_REALTIME);
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

esp_err_t monitor_smu_command_wait(uint8_t spindle, uint8_t opcode,
                                   uint8_t arg8, uint16_t arg16,
                                   int64_t max_wait_us, uint8_t *result_out)
{
    if (spindle >= NUM_SPINDLES) return ESP_ERR_INVALID_ARG;
    if (!s_ch[spindle].transport_ready) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_ch[spindle].mutex, portMAX_DELAY);
    esp_err_t err = smu_link_send_command_wait(&s_ch[spindle].link, opcode,
                                               arg8, arg16, max_wait_us, result_out);
    xSemaphoreGive(s_ch[spindle].mutex);
    return err;
}
