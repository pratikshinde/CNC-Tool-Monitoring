/*
 * calib.c
 *
 * The uncorrected ("nominal") value is recomputed from the raw pre-scaling
 * reading republished in the monitor snapshot, by running the same scaling
 * function the measurement path uses with gain=1, offset=0 and no
 * deadband. That matters for two reasons: the fit stays exactly consistent
 * with whatever scaling.c does (including the 4-20 mA live zero and the
 * 0-10 V divider), and calibration never has to touch the ADC, so it
 * cannot contend with the measurement loop for the I2C bus.
 */

#include "calib.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

#include "board.h"
#include "config_store.h"
#include "monitor.h"
#include "scaling.h"

static const char *TAG = "calib";

/* Two points closer together than this in engineering units cannot define
 * a trustworthy slope — the fit would amplify the operator's reading error
 * enormously. Expressed as a fraction of the configured sensor span. */
#define MIN_POINT_SEPARATION_FRAC 0.05f

static calib_point_t s_points[NUM_SPINDLES][CALIB_TARGET_COUNT][CALIB_MAX_POINTS];

/* ============================================================
 * Uncorrected readings
 * ============================================================ */

static float nominal_pressure(const pressure_cfg_t *cfg, float adc_volts)
{
    /* Same config, correction neutralised. */
    pressure_cfg_t raw = *cfg;
    raw.gain_correction   = 1.0f;
    raw.offset_correction = 0.0f;

    float value = 0.0f;
    pressure_scale(&raw, adc_volts, &value);
    return value;
}

static float nominal_current(const current_cfg_t *cfg, float burden_vrms)
{
    current_cfg_t raw = *cfg;
    raw.gain_correction = 1.0f;
    raw.noload_cutoff_a = 0.0f;   /* the deadband must not hide the point */

    return current_scale(&raw, burden_vrms);
}

static void live_values(uint8_t spindle, calib_target_t target,
                        float *nominal, float *corrected, float *raw)
{
    monitor_snapshot_t snap;
    monitor_get_snapshot(&snap);

    const app_config_t *cfg = config_get();
    const spindle_snapshot_t *sp = &snap.spindle[spindle];

    if (target == CALIB_PRESSURE) {
        *nominal   = nominal_pressure(&cfg->spindle[spindle].pressure,
                                      sp->pressure_adc_volts);
        *corrected = sp->pressure;
        *raw       = sp->pressure_loop_ma;
    } else {
        *nominal   = nominal_current(&cfg->spindle[spindle].current,
                                     sp->burden_vrms);
        *corrected = sp->current_a;
        *raw       = sp->burden_vrms;
    }
}

/* ============================================================
 * Capture / inspect / reset
 * ============================================================ */

esp_err_t calib_capture(uint8_t spindle, calib_target_t target,
                        uint8_t index, float reference)
{
    if (spindle >= NUM_SPINDLES || target >= CALIB_TARGET_COUNT ||
        index >= CALIB_MAX_POINTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!isfinite(reference)) return ESP_ERR_INVALID_ARG;

    float nominal, corrected, raw;
    live_values(spindle, target, &nominal, &corrected, &raw);

    s_points[spindle][target][index] = (calib_point_t){
        .captured  = true,
        .nominal   = nominal,
        .reference = reference,
    };

    ESP_LOGI(TAG, "S%u %s point %u: nominal %.3f, reference %.3f",
             spindle, (target == CALIB_PRESSURE) ? "pressure" : "current",
             index, nominal, reference);
    return ESP_OK;
}

void calib_reset(uint8_t spindle, calib_target_t target)
{
    if (spindle >= NUM_SPINDLES || target >= CALIB_TARGET_COUNT) return;
    memset(s_points[spindle][target], 0, sizeof(s_points[spindle][target]));
}

void calib_get_state(uint8_t spindle, calib_target_t target,
                     calib_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (spindle >= NUM_SPINDLES || target >= CALIB_TARGET_COUNT) return;

    memcpy(out->point, s_points[spindle][target], sizeof(out->point));
    live_values(spindle, target, &out->live_nominal, &out->live_corrected,
                &out->raw);
}

/* ============================================================
 * Solve and persist
 * ============================================================ */

esp_err_t calib_apply(uint8_t spindle, calib_target_t target,
                      cfg_result_t *reason)
{
    if (spindle >= NUM_SPINDLES || target >= CALIB_TARGET_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const calib_point_t *p = s_points[spindle][target];
    app_config_t working;
    config_get_copy(&working);

    if (target == CALIB_CURRENT) {
        /* Single point through the origin. */
        if (!p[0].captured) return ESP_ERR_INVALID_STATE;

        /* A point taken at (or near) no load carries no slope information
         * and would produce an absurd gain. */
        if (p[0].nominal <= 0.0f ||
            p[0].nominal < working.spindle[spindle].current.ct_primary_amps * 0.02f) {
            ESP_LOGW(TAG, "S%u current calibration point too small (%.3f A nominal)",
                     spindle, p[0].nominal);
            return ESP_ERR_INVALID_STATE;
        }
        if (p[0].reference <= 0.0f) return ESP_ERR_INVALID_STATE;

        working.spindle[spindle].current.gain_correction =
            p[0].reference / p[0].nominal;

        ESP_LOGI(TAG, "S%u current gain -> %.4f",
                 spindle, working.spindle[spindle].current.gain_correction);

    } else {
        pressure_cfg_t *pc = &working.spindle[spindle].pressure;

        if (p[0].captured && p[1].captured) {
            /* Two points: solve slope and intercept together. */
            float dn = p[1].nominal - p[0].nominal;
            float span = pc->sensor_max - pc->sensor_min;

            if (fabsf(dn) < fabsf(span) * MIN_POINT_SEPARATION_FRAC) {
                ESP_LOGW(TAG, "S%u pressure points too close: %.3f vs %.3f",
                         spindle, p[0].nominal, p[1].nominal);
                return ESP_ERR_INVALID_STATE;
            }

            float gain = (p[1].reference - p[0].reference) / dn;
            if (gain <= 0.0f) {
                /* A non-positive slope means the two points were entered
                 * the wrong way round, or the sensor is wired backwards.
                 * Either way, storing it would invert every reading. */
                ESP_LOGW(TAG, "S%u pressure fit produced non-positive gain %.4f",
                         spindle, gain);
                return ESP_ERR_INVALID_STATE;
            }

            pc->gain_correction   = gain;
            pc->offset_correction = p[0].reference - gain * p[0].nominal;

        } else if (p[0].captured) {
            /* One point: keep the slope, shift the intercept. Useful for a
             * pure zero-offset trim when only one reference is available. */
            pc->offset_correction =
                p[0].reference - pc->gain_correction * p[0].nominal;
        } else {
            return ESP_ERR_INVALID_STATE;
        }

        ESP_LOGI(TAG, "S%u pressure gain -> %.4f, offset -> %.3f",
                 spindle, pc->gain_correction, pc->offset_correction);
    }

    esp_err_t err = config_store_commit(&working, reason);
    if (err == ESP_OK) {
        calib_reset(spindle, target);
        /* Without this, the newly-fitted gain/offset lands in the
         * ESP32's NVS (config_get() reflects it immediately) but never
         * reaches the SMU that actually applies it to a measurement —
         * monitor.c's aggregate_task only pushes config to the SMU links
         * when this flag is set, same as a threshold edit does via
         * web.c's handle_thresholds_post(). Calibration is exactly the
         * same kind of config change and needs the same propagation. */
        monitor_config_changed();
    }
    return err;
}

/* ============================================================
 * Auto-zero
 *
 * V1 measured the CT channel's DC bias directly against the ESP32's own
 * ADC (analog_autozero()) and stored the result here. In V2 the ADC that
 * matters is on the SMU: this dispatches SMU_CMD_AUTOZERO over the I2C
 * link and waits for the SMU to confirm it, via monitor_smu_command_wait()
 * (monitor.c owns the smu_link_t instances; this file only ever reaches
 * them through that one door — see monitor.h).
 *
 * What this deliberately does NOT do: read the SMU's newly-measured
 * zero_offset_v back and resync it into the ESP32's own NVS copy. The
 * SMU applies the tare to its own local calibration immediately — the
 * measurement is not lost — but the two calibration stores (ESP32 NVS
 * canonical, SMU Data Flash autonomous, per SMU_HARDWARE_REQUIREMENTS.md
 * §3.1) go out of sync for this one field until the next full config
 * push. Building a clean read-back-and-resync path is a real piece of
 * work (SMU_REG_CONFIG's read side needs to expose live/applied values,
 * not just whatever was last staged — see smu_mock.c's own read handler,
 * which currently returns exactly that) and deserves designing on its
 * own rather than folding into this pass. Flagged, not silently skipped.
 * ============================================================ */

/* Generous relative to the mock (resolves on its first poll) and to a
 * real SMU's own bias measurement, which is a handful of ADC samples,
 * not a mechanical operation — this bounds "did the command even reach
 * the SMU", not the physics of the measurement itself. */
#define AUTOZERO_WAIT_US   (500 * 1000)

esp_err_t calib_autozero(uint8_t spindle)
{
    if (spindle >= NUM_SPINDLES) return ESP_ERR_INVALID_ARG;

    uint8_t result = 0xFF;
    esp_err_t err = monitor_smu_command_wait(spindle, SMU_CMD_AUTOZERO, 0, 0,
                                             AUTOZERO_WAIT_US, &result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "S%u auto-zero command failed: %s", spindle, esp_err_to_name(err));
        return err;
    }
    if (result != SMU_RESULT_OK) {
        ESP_LOGW(TAG, "S%u auto-zero rejected by SMU (result=%u)", spindle, result);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "S%u auto-zero acknowledged by SMU", spindle);
    return ESP_OK;
}
