/*
 * smu_mock.c — see smu_mock.h.
 *
 * Synthetic signal generator: a repeating profile (spin-up, cutting,
 * coast-down, stopped) driven off elapsed wall-clock time, offset by
 * spindle_index so the two mock spindles do not look identical. It is
 * deterministic on purpose — a mock whose behaviour changes between runs
 * is harder to debug against, not more realistic.
 */

#include "smu_mock.h"

#include <math.h>
#include <string.h>

#include "esp_timer.h"

#include "smu_pack.h"

/* --- Cycle profile, seconds --------------------------------------------
 * A full lap: spin-up, a cut long enough to close several cycles worth
 * of wear-baseline learning at the default 20-cycle window if left
 * running, coast-down, a pause. */
#define PHASE_SPINUP_S      3.0f
#define PHASE_CUT_S        20.0f
#define PHASE_COAST_S       4.0f
#define PHASE_STOP_S        3.0f
#define PHASE_TOTAL_S      (PHASE_SPINUP_S + PHASE_CUT_S + PHASE_COAST_S + PHASE_STOP_S)

#define SIM_RPM_TARGET   3000.0f
#define SIM_CURRENT_A      12.0f
#define SIM_PRESSURE_BAR   45.0f

static float elapsed_seconds(const smu_mock_spindle_t *m, int64_t now_us)
{
    return (float)(now_us - m->sim_start_us) / 1.0e6f;
}

/* Fills rpm/current_a/pressure for the current instant. Deliberately
 * simple, closed-form functions of elapsed time — no accumulated state,
 * so the profile is exactly reproducible from sim_start_us alone. */
static void synthesize(const smu_mock_spindle_t *m, int64_t now_us,
                       float *rpm, float *current_a, float *pressure)
{
    float t = elapsed_seconds(m, now_us);
    /* Stagger the second spindle a quarter-cycle out of phase so the two
     * do not visibly move in lockstep on a dashboard. */
    float offset = (m->spindle_index == 0) ? 0.0f : (PHASE_TOTAL_S * 0.25f);
    float phase = fmodf(t + offset, PHASE_TOTAL_S);

    if (phase < PHASE_SPINUP_S) {
        float k = phase / PHASE_SPINUP_S;
        *rpm       = SIM_RPM_TARGET * k;
        *current_a = SIM_CURRENT_A * 0.6f * k;   /* spin-up draws less than a loaded cut here */
        *pressure  = SIM_PRESSURE_BAR * k;
    } else if (phase < PHASE_SPINUP_S + PHASE_CUT_S) {
        float ct = phase - PHASE_SPINUP_S;
        *rpm       = SIM_RPM_TARGET + sinf(t * 2.0f) * 35.0f;
        *current_a = SIM_CURRENT_A + sinf(t * 3.7f) * 0.5f + sinf(t * 0.6f) * 0.3f;
        *pressure  = SIM_PRESSURE_BAR + sinf(t * 1.3f) * 1.8f;
        (void)ct;
    } else if (phase < PHASE_SPINUP_S + PHASE_CUT_S + PHASE_COAST_S) {
        float k = 1.0f - (phase - PHASE_SPINUP_S - PHASE_CUT_S) / PHASE_COAST_S;
        *rpm       = SIM_RPM_TARGET * k;
        *current_a = SIM_CURRENT_A * 0.2f * k;   /* windage only, decaying */
        *pressure  = SIM_PRESSURE_BAR * k;
    } else {
        *rpm       = 0.0f;
        *current_a = 0.03f;   /* below any sane noload_cutoff_a */
        *pressure  = 1.5f;
    }
}

/* ============================================================
 * Telemetry: run the real state machine + alarm engine, then pack the
 * result into wire format.
 * ============================================================ */

static void step_and_build_telemetry(smu_mock_spindle_t *m, int64_t now_us,
                                     smu_telemetry_t *out)
{
    float rpm, current_a, pressure;
    synthesize(m, now_us, &rpm, &current_a, &pressure);

    bool rpm_stopped = rpm < 1.0f;
    /* Same cross-check FIRMWARE_DESIGN_SPEC.md describes for the real
     * SMU: real current with no rotation means the speed sensor failed,
     * not that the spindle stopped. */
    bool rpm_suspect = rpm_stopped &&
                       (current_a > m->cfg.sm.cut_detect_current_a) &&
                       !m->inject_current_fault;

    /* spindle_sm.c/alarm.c take milliseconds (they compile into the SMU
     * image too — see FIRMWARE_DESIGN_SPEC.md §4.1); this mock stays on
     * esp_timer's microseconds for its own signal-synthesis timing, since
     * that part is ESP32-only and never ports, so the conversion happens
     * right at the boundary into the two shared calls below. */
    uint32_t now_ms = (uint32_t)(now_us / 1000);

    spindle_sm_update(&m->sm, &m->cfg.sm, rpm, rpm_stopped && !rpm_suspect,
                      current_a, pressure, now_ms);

    if (m->sm.state == SPINDLE_CUTTING) {
        if (current_a > m->peak_hold) m->peak_hold = current_a;
    } else {
        m->peak_hold = 0.0f;
    }

    alarm_input_t ai = {0};
    ai.value[QTY_CURRENT]  = current_a;
    ai.value[QTY_PRESSURE] = pressure;
    ai.value[QTY_RPM]      = rpm;
    ai.quantity_faulted[QTY_CURRENT]  = m->inject_current_fault;
    ai.quantity_faulted[QTY_PRESSURE] = m->inject_pressure_fault;
    ai.quantity_faulted[QTY_RPM]      = rpm_suspect;

    alarm_update(&m->alarm, &m->cfg, &ai, m->sm.monitoring_armed, now_ms);

    bool cycle_closed = false;
    cycle_summary_t cyc;
    if (spindle_sm_take_cycle(&m->sm, &cyc)) {
        alarm_on_cycle_end(&m->alarm, &m->cfg.wear, cyc.mean_current);
        m->cycle_count++;
        m->last_cycle_ms      = cyc.duration_ms;
        m->last_cycle_mean_a  = cyc.mean_current;
        m->last_cycle_peak_a  = cyc.peak_current;
        cycle_closed = true;
    }

    memset(out, 0, sizeof(*out));
    out->seq = m->inject_freeze_seq ? m->seq : (uint16_t)(m->seq + 1);
    if (!m->inject_freeze_seq) m->seq++;

    uint16_t status = 0;
    if (m->sm.monitoring_armed)      status |= SMU_STATUS_ARMED;
    if (m->sm.state == SPINDLE_CUTTING) status |= SMU_STATUS_MACHINE_RUN;
    if (!m->inject_link_down)        status |= SMU_STATUS_HEALTHY;
    if (m->alarm.any_alarm)          status |= SMU_STATUS_ANY_ALARM;
    if (m->alarm.any_warning)        status |= SMU_STATUS_ANY_WARNING;
    if (m->alarm.breakage)           status |= SMU_STATUS_BREAKAGE;
    if (m->alarm.crash)              status |= SMU_STATUS_CRASH;
    if (m->alarm.trend)              status |= SMU_STATUS_TREND;
    if (cycle_closed)                status |= SMU_STATUS_CYCLE_CLOSED;
    for (int q = 0; q < QTY_COUNT && !(status & SMU_STATUS_ANY_LATCHED); q++) {
        for (int b = 0; b < BAND_COUNT; b++) {
            if (m->alarm.bands[q][b].latched) { status |= SMU_STATUS_ANY_LATCHED; break; }
        }
    }
    out->status_flags = status;

    uint16_t diag = 0;
    if (m->inject_current_fault)  diag |= SMU_DIAG_CURRENT_SENSOR;
    if (m->inject_pressure_fault) diag |= SMU_DIAG_PRESSURE_SENSOR;
    if (rpm_suspect)               diag |= SMU_DIAG_RPM_SUSPECT;
    out->diag_flags = diag;

    out->state    = (uint8_t)m->sm.state;
    out->severity = (uint8_t)m->alarm.severity;

    out->current_a      = current_a;
    out->current_avg_a  = current_a;   /* mock does not model the 1s window separately */
    out->current_peak_a = m->peak_hold;
    out->pressure        = pressure;
    out->rpm              = rpm;

    for (int q = 0; q < SMU_QTY_COUNT; q++) {
        uint8_t active = 0, latched = 0;
        for (int b = 0; b < SMU_BAND_COUNT; b++) {
            if (m->alarm.bands[q][b].active)  active  |= (uint8_t)(1U << b);
            if (m->alarm.bands[q][b].latched) latched |= (uint8_t)(1U << b);
        }
        out->bands_active[q]  = active;
        out->bands_latched[q] = latched;
    }

    out->current_sensor  = m->inject_current_fault  ? SMU_SENSOR_OPEN : SMU_SENSOR_OK;
    out->pressure_sensor  = m->inject_pressure_fault ? SMU_SENSOR_OPEN : SMU_SENSOR_OK;

    out->output_state = 0;
    if (out->bands_active[QTY_CURRENT] || m->alarm.breakage || m->alarm.crash || m->alarm.trend)
        out->output_state |= SMU_OUT_CURRENT_FAULT;
    if (out->bands_active[QTY_PRESSURE])
        out->output_state |= SMU_OUT_PRESSURE_FAULT;
    if (out->bands_active[QTY_RPM] || rpm_suspect)
        out->output_state |= SMU_OUT_RPM_FAULT;
    if (!m->inject_link_down)
        out->output_state |= SMU_OUT_HEALTHY;

    /* Raw pre-scaling fields: the mock has no real ADC path to report
     * these from, so they are left at 0 rather than invented — an
     * honestly-absent value, not a plausible-looking fake one. */
    out->ct_burden_vrms   = 0.0f;
    out->pressure_adc_v   = 0.0f;

    out->cycle_count       = m->cycle_count;
    out->last_cycle_ms     = m->last_cycle_ms;
    out->last_cycle_mean_a = m->last_cycle_mean_a;
    out->last_cycle_peak_a = m->last_cycle_peak_a;

    out->timestamp_ms  = (uint32_t)(now_us / 1000);
    out->loop_worst_ms = 50;   /* the mock always "meets" its notional loop budget */

    out->crc16 = smu_crc16(out, SMU_CRC_PAYLOAD_LEN(smu_telemetry_t));
    if (m->inject_crc_corrupt) out->crc16 ^= 0xFFFF;
}

/* ============================================================
 * Config commit
 * ============================================================ */

static void handle_command(smu_mock_spindle_t *m, const smu_command_t *cmd)
{
    switch (cmd->opcode) {
    case SMU_CMD_CONFIG_COMMIT: {
        if (!m->staged_cfg_present) {
            m->ident.last_cmd_result = SMU_RESULT_CFG_INVALID;
            break;
        }
        smu_config_t wire;
        memcpy(&wire, m->staged_cfg, sizeof(wire));

        uint16_t computed = smu_crc16(&wire, SMU_CRC_PAYLOAD_LEN(smu_config_t));
        if (computed != wire.crc16) {
            m->ident.last_cmd_result = SMU_RESULT_BAD_CRC;
            break;
        }

        spindle_cfg_t candidate;
        uint8_t spindle_index_out = 0, mains_hz_out = 0;
        smu_config_unpack(&wire, &candidate, &spindle_index_out, &mains_hz_out);

        cfg_result_t v = spindle_cfg_validate(&candidate);
        if (v != CFG_OK) {
            m->ident.last_cmd_result = SMU_RESULT_CFG_INVALID;
            break;
        }

        m->cfg = candidate;
        m->staged_cfg_present = false;
        m->ident.cfg_schema_version = wire.schema_version;
        m->ident.last_cmd_result = SMU_RESULT_OK;
        break;
    }
    case SMU_CMD_CONFIG_ABORT:
        m->staged_cfg_present = false;
        m->ident.last_cmd_result = SMU_RESULT_OK;
        break;
    case SMU_CMD_ACKNOWLEDGE:
        alarm_acknowledge(&m->alarm);
        m->ident.last_cmd_result = SMU_RESULT_OK;
        break;
    case SMU_CMD_AUTOZERO:
    case SMU_CMD_RESET_TOOL_STATS:
    case SMU_CMD_RELEARN_BASELINE:
    case SMU_CMD_SOFT_RESET:
        /* No real effect to model for these in the mock; acknowledged so
         * the caller's push_config-style poll loop still completes. */
        m->ident.last_cmd_result = SMU_RESULT_OK;
        break;
    default:
        m->ident.last_cmd_result = SMU_RESULT_BAD_OPCODE;
        break;
    }
    m->ident.last_cmd_nonce = cmd->nonce;
}

/* ============================================================
 * Transport implementation
 * ============================================================ */

static esp_err_t mock_read(void *ctx, uint16_t reg, void *out, size_t len)
{
    smu_mock_spindle_t *m = (smu_mock_spindle_t *)ctx;
    if (m->inject_link_down) return ESP_ERR_TIMEOUT;

    if (reg == SMU_REG_IDENT) {
        if (len > sizeof(m->ident)) return ESP_ERR_INVALID_SIZE;
        m->ident.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
        memcpy(out, &m->ident, len);
        return ESP_OK;
    }
    if (reg == SMU_REG_TELEMETRY) {
        if (len > sizeof(smu_telemetry_t)) return ESP_ERR_INVALID_SIZE;
        smu_telemetry_t telem;
        step_and_build_telemetry(m, esp_timer_get_time(), &telem);
        memcpy(out, &telem, len);
        return ESP_OK;
    }
    if (reg == SMU_REG_CONFIG) {
        if (len > sizeof(m->staged_cfg)) return ESP_ERR_INVALID_SIZE;
        /* Reading back the config register returns whatever was last
         * staged, matching a real SMU exposing its own memory-mapped
         * block for read verification. */
        memcpy(out, m->staged_cfg, len);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t mock_write(void *ctx, uint16_t reg, const void *data, size_t len)
{
    smu_mock_spindle_t *m = (smu_mock_spindle_t *)ctx;
    if (m->inject_link_down) return ESP_ERR_TIMEOUT;

    if (reg == SMU_REG_CONFIG) {
        if (len > sizeof(m->staged_cfg)) return ESP_ERR_INVALID_SIZE;
        memcpy(m->staged_cfg, data, len);
        m->staged_cfg_present = true;
        return ESP_OK;
    }
    if (reg == SMU_REG_COMMAND) {
        if (len != sizeof(smu_command_t)) return ESP_ERR_INVALID_SIZE;
        smu_command_t cmd;
        memcpy(&cmd, data, len);
        uint16_t computed = smu_crc16(&cmd, SMU_CRC_PAYLOAD_LEN(smu_command_t));
        if (computed != cmd.crc16) {
            /* A corrupted command still gets a nonce echo — the master
             * needs to see SOME result to stop retrying — but with a
             * result code that says the frame itself was untrustworthy. */
            m->ident.last_cmd_result = SMU_RESULT_BAD_CRC;
            m->ident.last_cmd_nonce  = cmd.nonce;
            return ESP_OK;
        }
        handle_command(m, &cmd);
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static void mock_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    /* A real transport delay here would block the caller for real; the
     * mock has nothing asynchronous to wait for (handle_command() above
     * already applied the config synchronously inside the write call),
     * so there is deliberately nothing to do. smu_link_push_config()
     * against the mock therefore resolves on its very first ident poll. */
    (void)us;
}

/* ============================================================
 * Public API
 * ============================================================ */

smu_transport_t smu_mock_init(smu_mock_spindle_t *m, uint8_t spindle_index,
                              int64_t now_us)
{
    memset(m, 0, sizeof(*m));
    m->spindle_index = spindle_index;
    spindle_cfg_set_defaults(&m->cfg, spindle_index);
    spindle_sm_init(&m->sm, (uint32_t)(now_us / 1000));
    alarm_init(&m->alarm);
    m->sim_start_us = now_us;

    m->ident.magic         = SMU_PROTO_MAGIC;
    m->ident.proto_version = SMU_PROTO_VERSION;
    m->ident.spindle_index = spindle_index;
    m->ident.fw_version    = (0 << 8) | 1;   /* mock "firmware" 0.1 */
    m->ident.reset_cause   = SMU_RESET_POWER_ON;
    m->ident.cfg_schema_version = 0;   /* nothing committed yet */

    smu_transport_t t = { .read = mock_read, .write = mock_write,
                          .delay_us = mock_delay, .ctx = m };
    return t;
}

void smu_mock_set_link_down(smu_mock_spindle_t *m, bool down)
{ m->inject_link_down = down; }

void smu_mock_set_crc_corrupt(smu_mock_spindle_t *m, bool corrupt)
{ m->inject_crc_corrupt = corrupt; }

void smu_mock_set_freeze_seq(smu_mock_spindle_t *m, bool freeze)
{ m->inject_freeze_seq = freeze; }

void smu_mock_set_current_fault(smu_mock_spindle_t *m, bool faulted)
{ m->inject_current_fault = faulted; }

void smu_mock_set_pressure_fault(smu_mock_spindle_t *m, bool faulted)
{ m->inject_pressure_fault = faulted; }
