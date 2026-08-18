/*
 * test_main.c — host tests for the pure logic modules.
 *
 * These compile and run on a PC with plain gcc, no ESP-IDF and no
 * hardware. They cover the parts of the system where a mistake is silent:
 * scaling arithmetic, the threshold state machine's delay/hysteresis/latch
 * interactions, the spindle state machine's arming rules, and the output
 * mapping.
 *
 * Build and run:
 *     cd host_test && make
 *
 * These are not a substitute for bench testing. They prove the logic does
 * what it was written to do; they cannot prove it is the right logic for a
 * real machine. That is what Phase 6 is for.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "app_config.h"
#include "scaling.h"
#include "spindle_sm.h"
#include "alarm.h"
#include "smu_proto.h"
#include "smu_pack.h"
#include "smu_link.h"
#include "job_template.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                            \
    do {                                                                 \
        if (cond) { g_pass++; }                                          \
        else {                                                           \
            g_fail++;                                                    \
            printf("  FAIL %s:%d  " fmt "\n", __FILE__, __LINE__,        \
                   ##__VA_ARGS__);                                       \
        }                                                                \
    } while (0)

#define CLOSE(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))

static void section(const char *name)
{
    printf("\n== %s\n", name);
}

/* ============================================================
 * Scaling
 * ============================================================ */

static void test_pressure_scaling(void)
{
    section("pressure scaling");

    pressure_cfg_t cfg = {
        .mode = PRESSURE_INPUT_4_20MA,
        .sensor_min = 0.0f, .sensor_max = 100.0f,
        .gain_correction = 1.0f, .offset_correction = 0.0f,
    };

    float out;

    /* 100 ohm burden: 4 mA -> 0.400 V, 12 mA -> 1.200 V, 20 mA -> 2.000 V */
    CHECK(pressure_scale(&cfg, 0.400f, &out) == SENSOR_OK && CLOSE(out, 0.0f, 0.01f),
          "4 mA should be 0 bar, got %.3f", out);
    CHECK(pressure_scale(&cfg, 1.200f, &out) == SENSOR_OK && CLOSE(out, 50.0f, 0.01f),
          "12 mA should be 50 bar, got %.3f", out);
    CHECK(pressure_scale(&cfg, 2.000f, &out) == SENSOR_OK && CLOSE(out, 100.0f, 0.01f),
          "20 mA should be 100 bar, got %.3f", out);

    /* Loop faults must be reported, not scaled (AI-R3). */
    CHECK(pressure_scale(&cfg, 0.000f, &out) == SENSOR_OPEN,
          "0 mA should report SENSOR_OPEN");
    CHECK(pressure_scale(&cfg, 0.300f, &out) == SENSOR_OPEN,
          "3 mA should report SENSOR_OPEN");
    CHECK(pressure_scale(&cfg, 2.200f, &out) == SENSOR_OVERRANGE,
          "22 mA should report SENSOR_OVERRANGE");

    /* Just inside the live-zero band is a valid reading, not a fault. */
    CHECK(pressure_scale(&cfg, 0.380f, &out) == SENSOR_OK,
          "3.8 mA should be OK (slightly below zero, within tolerance)");

    /* Non-zero sensor_min must offset correctly. */
    cfg.sensor_min = -1.0f;
    cfg.sensor_max = 9.0f;
    CHECK(pressure_scale(&cfg, 1.200f, &out) == SENSOR_OK && CLOSE(out, 4.0f, 0.01f),
          "midspan of -1..9 should be 4, got %.3f", out);

    /* Gain and offset corrections. */
    cfg.sensor_min = 0.0f; cfg.sensor_max = 100.0f;
    cfg.gain_correction = 1.10f;
    cfg.offset_correction = -2.0f;
    CHECK(pressure_scale(&cfg, 1.200f, &out) == SENSOR_OK && CLOSE(out, 53.0f, 0.01f),
          "50 * 1.1 - 2 should be 53, got %.3f", out);

    /* 0-10 V mode through the fitted divider. */
    pressure_cfg_t vcfg = {
        .mode = PRESSURE_INPUT_0_10V,
        .sensor_min = 0.0f, .sensor_max = 250.0f,
        .gain_correction = 1.0f, .offset_correction = 0.0f,
    };
    float half_scale_adc = 5.0f * PRESSURE_DIV_RATIO;
    CHECK(pressure_scale(&vcfg, half_scale_adc, &out) == SENSOR_OK &&
          CLOSE(out, 125.0f, 0.5f),
          "5 V should be 125, got %.3f", out);

    float over_adc = 11.0f * PRESSURE_DIV_RATIO;
    CHECK(pressure_scale(&vcfg, over_adc, &out) == SENSOR_OVERRANGE,
          "11 V should report SENSOR_OVERRANGE");
}

static void test_current_scaling(void)
{
    section("current scaling");

    current_cfg_t cfg = {
        .ct_primary_amps = 50.0f,
        .ct_secondary_ma = 50.0f,
        .gain_correction = 1.0f,
    };

    /* 50 A primary -> 50 mA secondary -> 50 mA across the fitted burden.
     * Derived from CT_BURDEN_OHM rather than hard-coded, so this test does
     * not have to change when the board's burden resistor does. */
    float vrms_at_full = 0.050f * CT_BURDEN_OHM;
    CHECK(CLOSE(current_scale(&cfg, vrms_at_full), 50.0f, 0.01f),
          "full scale should be 50 A, got %.3f", current_scale(&cfg, vrms_at_full));

    CHECK(CLOSE(current_scale(&cfg, vrms_at_full / 2.0f), 25.0f, 0.01f),
          "half scale should be 25 A");

    CHECK(CLOSE(current_scale(&cfg, 0.0f), 0.0f, 0.001f),
          "zero volts should be zero amps");

    /* Negative RMS is not physical; must clamp rather than propagate. */
    CHECK(current_scale(&cfg, -0.5f) == 0.0f,
          "negative burden voltage should clamp to zero");

    /* A different CT ratio. */
    cfg.ct_primary_amps = 100.0f;
    cfg.ct_secondary_ma = 50.0f;
    CHECK(CLOSE(current_scale(&cfg, vrms_at_full), 100.0f, 0.01f),
          "100:0.05 CT at full secondary should read 100 A");

    /* Guard against a divide-by-zero from a bad config. */
    cfg.ct_secondary_ma = 0.0f;
    CHECK(current_scale(&cfg, vrms_at_full) == 0.0f,
          "zero secondary rating must not produce inf/nan");

    /* --- No-load cutoff ----------------------------------------------
     * The noise floor of an RMS is always positive, so without a deadband
     * an idle spindle never reads zero. This is the fix for the ~0.02 A
     * observed at no load on the bench. */
    current_cfg_t dead = {
        .ct_primary_amps  = 50.0f,
        .ct_secondary_ma  = 50.0f,
        .gain_correction  = 1.0f,
        .noload_cutoff_a  = 0.5f,
    };

    /* 0.2 A of noise is below the 0.5 A deadband -> exactly zero. */
    float vrms_noise = (0.2f / 50.0f) * 0.050f * CT_BURDEN_OHM;
    CHECK(current_scale(&dead, vrms_noise) == 0.0f,
          "a reading below the cutoff must be exactly zero, got %.4f",
          current_scale(&dead, vrms_noise));

    /* Real load well above the deadband passes through unchanged. */
    CHECK(CLOSE(current_scale(&dead, vrms_at_full), 50.0f, 0.01f),
          "a reading far above the cutoff must be unaffected");

    /* The deadband must suppress and not merely subtract: a value just
     * above the threshold keeps its full magnitude rather than being
     * shifted down by the cutoff. */
    float vrms_just_over = (0.6f / 50.0f) * 0.050f * CT_BURDEN_OHM;
    CHECK(CLOSE(current_scale(&dead, vrms_just_over), 0.6f, 0.01f),
          "just above the cutoff must read its true value, not value-cutoff");

    /* A zero cutoff disables the deadband entirely. */
    dead.noload_cutoff_a = 0.0f;
    CHECK(current_scale(&dead, vrms_noise) > 0.0f,
          "a zero cutoff must leave small readings alone");
}

static void test_rpm_scaling(void)
{
    section("rpm scaling");

    /* 1 PPR, 1000 rpm -> 16.667 rev/s -> 60 000 us between pulses. */
    CHECK(CLOSE(rpm_from_interval(60000.0f, 1), 1000.0f, 0.1f),
          "60 ms interval at 1 PPR should be 1000 rpm, got %.2f",
          rpm_from_interval(60000.0f, 1));

    /* 4 PPR quarters the interval for the same speed. */
    CHECK(CLOSE(rpm_from_interval(15000.0f, 4), 1000.0f, 0.1f),
          "15 ms at 4 PPR should be 1000 rpm");

    /* Count mode: 100 pulses in 100 ms at 1 PPR = 60 000 rpm. */
    CHECK(CLOSE(rpm_from_count(100, 100.0f, 1), 60000.0f, 1.0f),
          "100 pulses / 100 ms should be 60000 rpm, got %.1f",
          rpm_from_count(100, 100.0f, 1));

    CHECK(CLOSE(rpm_from_count(20, 100.0f, 1), 12000.0f, 1.0f),
          "20 pulses / 100 ms should be 12000 rpm");

    /* Degenerate inputs must not divide by zero. */
    CHECK(rpm_from_interval(0.0f, 1) == 0.0f,   "zero interval -> 0");
    CHECK(rpm_from_interval(1000.0f, 0) == 0.0f,"zero PPR -> 0");
    CHECK(rpm_from_count(10, 0.0f, 1) == 0.0f,  "zero gate -> 0");
}

/* ============================================================
 * Configuration validation
 * ============================================================ */

static void test_config_validation(void)
{
    section("config validation");

    app_config_t cfg;
    app_config_set_defaults(&cfg);

    CHECK(app_config_validate(&cfg, NULL) == CFG_OK,
          "factory defaults must validate");
    CHECK(app_config_check(&cfg), "factory defaults must pass CRC check");

    /* CRC must catch a single-bit change anywhere in the blob. */
    app_config_t tampered = cfg;
    tampered.spindle[0].sm.start_rpm += 1.0f;
    CHECK(!app_config_check(&tampered), "CRC must detect a modified field");

    /* Band ordering. */
    app_config_t bad = cfg;
    bad.spindle[0].bands[QTY_CURRENT][BAND_HI].limit = 999.0f;  /* above HiHi */
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_BAND_ORDER,
          "Hi above HiHi must be rejected");

    /* Disabled bands must not block a save (AL-R1). */
    bad = cfg;
    bad.spindle[0].bands[QTY_CURRENT][BAND_LO].enabled = false;
    bad.spindle[0].bands[QTY_CURRENT][BAND_LO].limit = 9999.0f;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_OK,
          "a nonsense limit on a DISABLED band must not block the save");

    /* Sensor span. */
    bad = cfg;
    bad.spindle[0].pressure.sensor_max = bad.spindle[0].pressure.sensor_min;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_SENSOR_SPAN,
          "zero sensor span must be rejected");

    /* State machine separation. */
    bad = cfg;
    bad.spindle[0].sm.cut_detect_current_a = bad.spindle[0].sm.idle_current_a;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_SM_THRESHOLD,
          "cut-detect equal to idle must be rejected (chatter risk)");

    /* PPR range. */
    bad = cfg;
    bad.spindle[0].rpm.pulses_per_rev = 0;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_PPR_RANGE,
          "zero PPR must be rejected");

    bad = cfg;
    bad.spindle[0].rpm.pulses_per_rev = 2000;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_PPR_RANGE,
          "PPR above 1024 must be rejected");

    /* Burst size. */
    bad = cfg;
    bad.spindle[0].current.rms_burst_samples = 4;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_BURST_RANGE,
          "a 4-sample RMS burst must be rejected");

    /* SMU output minimum pulse width (DO-R4). */
    bad = cfg;
    bad.spindle[0].min_pulse_ms = 0;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_PERIOD_RANGE,
          "min_pulse_ms of 0 must be rejected (defeats DO-R4)");

    bad = cfg;
    bad.spindle[0].min_pulse_ms = 60001;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_PERIOD_RANGE,
          "min_pulse_ms above 60s must be rejected");

    /* A disabled spindle's settings should not block the save. */
    bad = cfg;
    bad.spindle[1].enabled = false;
    bad.spindle[1].rpm.pulses_per_rev = 0;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_OK,
          "a disabled spindle's bad settings must not block the save");

    /* No-load cutoff: a deadband may swallow the noise floor, but must not
     * be allowed to hide real cutting current. */
    bad = cfg;
    bad.spindle[0].current.noload_cutoff_a = -0.1f;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_CT_RANGE,
          "a negative no-load cutoff must be rejected");

    bad = cfg;
    bad.spindle[0].current.noload_cutoff_a =
        bad.spindle[0].current.ct_primary_amps * 0.5f;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_CT_RANGE,
          "a cutoff at 50%% of CT rating must be rejected (would hide real load)");

    bad = cfg;
    bad.spindle[0].current.noload_cutoff_a =
        bad.spindle[0].current.ct_primary_amps * 0.05f;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_OK,
          "a cutoff at exactly 5%% of CT rating must be accepted");

    /* --- Adaptive wear baseline (schema v8, FIRMWARE_DESIGN_SPEC.md §3.8)
     * ------------------------------------------------------------------
     * The baseline's validity gate (spindle_config.c) only behaves as
     * documented if k_warn < k_alarm and floor_pct < ceiling_pct — these
     * checks exist so a config that would silently defeat that gate is
     * rejected at commit time, not discovered at the next power cycle. */

    bad = cfg;
    bad.spindle[0].wear.adaptive_k_warn = 5.0f;
    bad.spindle[0].wear.adaptive_k_alarm = 3.0f;   /* warn above alarm */
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_WEAR_BASELINE,
          "adaptive_k_warn >= adaptive_k_alarm must be rejected");

    bad = cfg;
    bad.spindle[0].wear.adaptive_k_warn = 0.0f;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_WEAR_BASELINE,
          "adaptive_k_warn <= 0 must be rejected");

    bad = cfg;
    bad.spindle[0].wear.baseline_sigma_floor_pct = 25;
    bad.spindle[0].wear.baseline_sigma_ceiling_pct = 25;  /* floor == ceiling */
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_WEAR_BASELINE,
          "sigma floor equal to ceiling must be rejected (zero usable band)");

    bad = cfg;
    bad.spindle[0].wear.baseline_sigma_floor_pct = 0;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_WEAR_BASELINE,
          "sigma floor of 0%% must be rejected");

    bad = cfg;
    bad.spindle[0].wear.baseline_learn_cycles = 4;   /* below the 5 minimum */
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_WEAR_BASELINE,
          "fewer than 5 learning cycles must be rejected");

    bad = cfg;
    bad.spindle[0].wear.baseline_learn_cycles = 65;  /* above the 64 maximum */
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_WEAR_BASELINE,
          "more than 64 learning cycles must be rejected");

    /* A disabled spindle's baseline settings must not block the save,
     * same rule as every other per-spindle check above. */
    bad = cfg;
    bad.spindle[1].enabled = false;
    bad.spindle[1].wear.adaptive_k_warn = 0.0f;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_OK,
          "a disabled spindle's bad baseline settings must not block the save");

    /* --- Modbus ------------------------------------------------------- */

    bad = cfg;
    bad.system.modbus.baud = 100;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_MODBUS_RANGE,
          "baud below 300 must be rejected");

    bad = cfg;
    bad.system.modbus.baud = 500000;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_MODBUS_RANGE,
          "baud above 115200 must be rejected");

    bad = cfg;
    bad.system.modbus.stop_bits = 3;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_MODBUS_RANGE,
          "stop bits outside {1,2} must be rejected");

    bad = cfg;
    bad.system.modbus.slave_id = 0;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_MODBUS_RANGE,
          "slave ID 0 (broadcast) must be rejected");

    bad = cfg;
    bad.system.modbus.slave_id = 248;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_MODBUS_RANGE,
          "slave ID above 247 must be rejected");

    /* RTU is a fixed-8-data-bit protocol; 7 only exists for ASCII framing,
     * which this device does not implement. The old Arduino sketch let the
     * UI pick 7 or 8 with no such check — that was a latent bug, not a
     * feature to preserve. */
    bad = cfg;
    bad.system.modbus.rtu_enabled = true;
    bad.system.modbus.data_bits = 7;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_MODBUS_RANGE,
          "7 data bits with RTU enabled must be rejected");

    bad = cfg;
    bad.system.modbus.tcp_enabled = true;
    bad.system.modbus.tcp_port = 0;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_MODBUS_RANGE,
          "TCP port 0 with TCP enabled must be rejected");

    /* A disabled interface's stale parameters must not block the save. */
    bad = cfg;
    bad.system.modbus.rtu_enabled = false;
    bad.system.modbus.tcp_enabled = false;
    bad.system.modbus.tcp_port = 0;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_OK,
          "tcp_port 0 must not block the save when TCP is disabled");
}

/* ============================================================
 * Spindle state machine
 * ============================================================ */

/* spindle_sm_update()/alarm_update() take uint32_t milliseconds directly
 * now (FIRMWARE_DESIGN_SPEC.md §4.1) — MS() is kept as a documenting
 * identity cast rather than dropped, so every call site below still reads
 * as "this literal is a millisecond quantity". */
#define MS(x) ((uint32_t)(x))

static void test_spindle_state_machine(void)
{
    section("spindle state machine");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    const spindle_sm_cfg_t *sc = &cfg.spindle[0].sm;
    /* defaults: start 50 rpm, settle 500 ms, idle 3 A, cut 6 A, inhibit 500 ms */

    spindle_sm_t sm;
    uint32_t t = 0;
    spindle_sm_init(&sm, t);

    CHECK(sm.state == SPINDLE_STOPPED, "starts STOPPED");

    /* Spindle starts turning. */
    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 1.0f, 40.0f, t);
    CHECK(sm.state == SPINDLE_SPIN_UP, "turning -> SPIN_UP, got %s",
          spindle_state_str(sm.state));

    /* Heavy start-up current must NOT arm monitoring. This is the whole
     * point of the state machine. */
    t += MS(200);
    spindle_sm_update(&sm, sc, 1200.0f, false, 45.0f, 40.0f, t);
    CHECK(sm.state == SPINDLE_SPIN_UP, "still SPIN_UP before settle");
    CHECK(!sm.monitoring_armed, "must NOT be armed during spin-up surge");

    /* Settle period elapses. */
    t += MS(400);
    spindle_sm_update(&sm, sc, 1200.0f, false, 1.5f, 40.0f, t);
    CHECK(sm.state == SPINDLE_IDLE, "after settle -> IDLE, got %s",
          spindle_state_str(sm.state));
    CHECK(!sm.monitoring_armed, "IDLE must not be armed");

    /* Idle windage below the cut threshold stays IDLE. */
    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 4.0f, 40.0f, t);
    CHECK(sm.state == SPINDLE_IDLE, "4 A (below 6 A cut detect) stays IDLE");

    /* Tool enters material. */
    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 20.0f, 60.0f, t);
    CHECK(sm.state == SPINDLE_CUTTING, "above cut detect -> CUTTING");
    CHECK(!sm.monitoring_armed, "inhibit window must suppress arming on entry");

    /* Inhibit window expires. */
    t += MS(600);
    spindle_sm_update(&sm, sc, 1200.0f, false, 20.0f, 60.0f, t);
    CHECK(sm.monitoring_armed, "armed once the inhibit window has passed");

    /* Accumulate a few samples with a known peak. */
    for (int i = 0; i < 10; i++) {
        t += MS(100);
        spindle_sm_update(&sm, sc, 1200.0f, false, 20.0f, 60.0f, t);
    }
    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 30.0f, 60.0f, t);

    /* Tool leaves material. */
    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 1.0f, 40.0f, t);
    CHECK(sm.state == SPINDLE_IDLE, "current below idle -> IDLE, got %s",
          spindle_state_str(sm.state));
    CHECK(sm.cycle_completed, "leaving CUTTING must complete a cycle");
    CHECK(sm.cycle_count == 1, "cycle count should be 1, got %u", sm.cycle_count);

    cycle_summary_t cyc;
    CHECK(spindle_sm_take_cycle(&sm, &cyc), "cycle summary available");
    CHECK(CLOSE(cyc.peak_current, 30.0f, 0.01f),
          "peak should be 30 A, got %.2f", cyc.peak_current);
    CHECK(cyc.mean_current > 19.0f && cyc.mean_current < 22.0f,
          "mean should be near 20 A, got %.2f", cyc.mean_current);
    CHECK(!sm.monitoring_armed, "leaving CUTTING must disarm");

    /* cycle_completed must be a one-shot, not sticky — otherwise every
     * subsequent pass would log a duplicate cycle record. */
    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 1.0f, 40.0f, t);
    CHECK(!sm.cycle_completed, "cycle_completed must clear after one pass");

    /* Spindle stops. */
    t += MS(100);
    spindle_sm_update(&sm, sc, 0.0f, true, 0.5f, 20.0f, t);
    CHECK(sm.state == SPINDLE_COAST_DOWN, "stopping -> COAST_DOWN, got %s",
          spindle_state_str(sm.state));

    t += MS(100);
    spindle_sm_update(&sm, sc, 0.0f, true, 0.5f, 20.0f, t);
    CHECK(sm.state == SPINDLE_STOPPED, "at rest -> STOPPED, got %s",
          spindle_state_str(sm.state));

    /* A stop that happens mid-cut must still close the cycle out. */
    spindle_sm_init(&sm, t);
    t += MS(100); spindle_sm_update(&sm, sc, 1200.0f, false,  1.0f, 40.0f, t);
    t += MS(600); spindle_sm_update(&sm, sc, 1200.0f, false,  1.0f, 40.0f, t);
    t += MS(100); spindle_sm_update(&sm, sc, 1200.0f, false, 20.0f, 60.0f, t);
    CHECK(sm.state == SPINDLE_CUTTING, "set up a cut");
    t += MS(100); spindle_sm_update(&sm, sc, 0.0f, true, 0.0f, 0.0f, t);
    CHECK(sm.cycle_completed, "an e-stop mid-cut must still close the cycle");
}

/* ============================================================
 * Alarm engine
 * ============================================================ */

static void feed(alarm_state_t *st, const spindle_cfg_t *cfg,
                 float current, bool armed, uint32_t t)
{
    alarm_input_t in = {0};
    in.value[QTY_CURRENT]  = current;
    in.value[QTY_PRESSURE] = 50.0f;   /* mid-band, quiet */
    in.value[QTY_RPM]      = 1200.0f;
    alarm_update(st, cfg, &in, armed, t);
}

static void test_alarm_delays_and_hysteresis(void)
{
    section("alarm delays and hysteresis");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    spindle_cfg_t *sc = &cfg.spindle[0];

    /* Simplify: only the current Hi band, 500 ms on-delay, 1000 ms off,
     * limit 35 A, hysteresis 0.5 A, non-latching. */
    for (int q = 0; q < QTY_COUNT; q++)
        for (int b = 0; b < BAND_COUNT; b++)
            sc->bands[q][b].enabled = false;

    sc->bands[QTY_CURRENT][BAND_HI].enabled = true;
    sc->bands[QTY_CURRENT][BAND_HI].limit = 35.0f;
    sc->bands[QTY_CURRENT][BAND_HI].hysteresis = 0.5f;
    sc->bands[QTY_CURRENT][BAND_HI].on_delay_ms = 500;
    sc->bands[QTY_CURRENT][BAND_HI].off_delay_ms = 1000;
    sc->bands[QTY_CURRENT][BAND_HI].latching = false;
    sc->wear.breakage_enabled = false;
    sc->wear.crash_enabled = false;

    alarm_state_t st;
    alarm_init(&st);
    uint32_t t = 0;

    const band_state_t *hi = &st.bands[QTY_CURRENT][BAND_HI];

    /* Below limit: quiet. */
    feed(&st, sc, 20.0f, true, t);
    CHECK(!hi->active, "below limit must be inactive");

    /* Cross the limit — must not fire immediately. */
    t += MS(100); feed(&st, sc, 40.0f, true, t);
    CHECK(hi->raw, "raw comparison should be true immediately");
    CHECK(!hi->active, "on-delay must suppress the immediate assert");

    /* Still inside the delay. */
    t += MS(300); feed(&st, sc, 40.0f, true, t);
    CHECK(!hi->active, "still within the 500 ms on-delay at 400 ms");

    /* Delay elapses. */
    t += MS(200); feed(&st, sc, 40.0f, true, t);
    CHECK(hi->active, "must assert once the on-delay has elapsed");
    CHECK(CLOSE(hi->value_at_trip, 40.0f, 0.01f),
          "value at trip should be recorded, got %.2f", hi->value_at_trip);

    /* A transient dip inside the hysteresis band must not clear it. */
    t += MS(100); feed(&st, sc, 34.8f, true, t);   /* limit-0.2, inside hyst */
    CHECK(hi->raw, "34.8 A is inside hysteresis, raw must stay true");
    t += MS(2000); feed(&st, sc, 34.8f, true, t);
    CHECK(hi->active, "must NOT clear while inside the hysteresis band");

    /* Drop clearly below limit-hysteresis. */
    t += MS(100); feed(&st, sc, 30.0f, true, t);
    CHECK(!hi->raw, "30 A is below limit-hysteresis, raw must go false");
    CHECK(hi->active, "off-delay must hold it active initially");

    t += MS(500); feed(&st, sc, 30.0f, true, t);
    CHECK(hi->active, "still within the 1000 ms off-delay at 500 ms");

    t += MS(600); feed(&st, sc, 30.0f, true, t);
    CHECK(!hi->active, "must clear once the off-delay has elapsed");

    /* A brief spike shorter than the on-delay must be rejected entirely.
     * This is the transient-rejection requirement (AL-R2). */
    alarm_init(&st);
    t = 0;
    feed(&st, sc, 20.0f, true, t);
    t += MS(100); feed(&st, sc, 50.0f, true, t);
    t += MS(200); feed(&st, sc, 50.0f, true, t);   /* 200 ms < 500 ms */
    t += MS(100); feed(&st, sc, 20.0f, true, t);
    t += MS(100); feed(&st, sc, 20.0f, true, t);
    CHECK(!hi->active, "a 300 ms spike must not trip a 500 ms on-delay");
}

static void test_alarm_arming_and_latching(void)
{
    section("alarm arming and latching");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    spindle_cfg_t *sc = &cfg.spindle[0];

    for (int q = 0; q < QTY_COUNT; q++)
        for (int b = 0; b < BAND_COUNT; b++)
            sc->bands[q][b].enabled = false;

    sc->bands[QTY_CURRENT][BAND_HIHI].enabled = true;
    sc->bands[QTY_CURRENT][BAND_HIHI].limit = 45.0f;
    sc->bands[QTY_CURRENT][BAND_HIHI].hysteresis = 0.5f;
    sc->bands[QTY_CURRENT][BAND_HIHI].on_delay_ms = 100;
    sc->bands[QTY_CURRENT][BAND_HIHI].off_delay_ms = 500;
    sc->bands[QTY_CURRENT][BAND_HIHI].latching = true;
    sc->wear.breakage_enabled = false;
    sc->wear.crash_enabled = false;

    alarm_state_t st;
    alarm_init(&st);
    uint32_t t = 0;

    const band_state_t *hihi = &st.bands[QTY_CURRENT][BAND_HIHI];

    /* DISARMED: a gross overload must not assert. This is what stops
     * spin-up surge from alarming. */
    feed(&st, sc, 100.0f, false, t);
    t += MS(1000); feed(&st, sc, 100.0f, false, t);
    CHECK(!hihi->active, "must not assert while disarmed");
    CHECK(!st.any_alarm, "any_alarm must be false while disarmed");

    /* ARMED: the same value now asserts and latches. */
    t += MS(100); feed(&st, sc, 100.0f, true, t);
    t += MS(200); feed(&st, sc, 100.0f, true, t);
    CHECK(hihi->active, "must assert once armed");
    CHECK(hihi->latched, "a latching band must latch");
    CHECK(st.any_alarm, "any_alarm must be true");
    CHECK(st.severity == SEV_ALARM, "severity should be SEV_ALARM, got %s",
          severity_str(st.severity));

    /* Condition clears — the latch must hold. */
    t += MS(100);  feed(&st, sc, 10.0f, true, t);
    t += MS(1000); feed(&st, sc, 10.0f, true, t);
    CHECK(!hihi->active, "the condition itself has cleared");
    CHECK(hihi->latched, "but the latch must persist until acknowledged");
    CHECK(st.any_alarm, "any_alarm must stay true while latched");

    /* Acknowledge with the condition gone: the latch releases. */
    alarm_acknowledge(&st);
    feed(&st, sc, 10.0f, true, t);
    CHECK(!hihi->latched, "acknowledging a cleared condition releases it");
    CHECK(!st.any_alarm, "any_alarm clears after acknowledgement");

    /* Acknowledging while STILL in alarm must not clear it. */
    t += MS(100); feed(&st, sc, 100.0f, true, t);
    t += MS(200); feed(&st, sc, 100.0f, true, t);
    CHECK(hihi->latched, "re-tripped and latched");
    alarm_acknowledge(&st);
    feed(&st, sc, 100.0f, true, t);
    CHECK(hihi->latched, "acknowledging an ACTIVE alarm must not silence it");
    CHECK(hihi->acknowledged, "but it must be recorded as acknowledged");

    /* Disarming must not clear an existing latch — an operator who has not
     * yet seen the alarm must still see it. */
    t += MS(100); feed(&st, sc, 100.0f, false, t);
    CHECK(hihi->latched, "disarming must not silently drop a latch");
}

static void test_sensor_fault_isolation(void)
{
    section("sensor fault isolation");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    spindle_cfg_t *sc = &cfg.spindle[0];

    for (int q = 0; q < QTY_COUNT; q++)
        for (int b = 0; b < BAND_COUNT; b++)
            sc->bands[q][b].enabled = false;

    /* Arm the pressure LoLo band — the one a dead 4-20 mA loop would trip. */
    sc->bands[QTY_PRESSURE][BAND_LOLO].enabled = true;
    sc->bands[QTY_PRESSURE][BAND_LOLO].limit = 10.0f;
    sc->bands[QTY_PRESSURE][BAND_LOLO].on_delay_ms = 100;
    sc->bands[QTY_PRESSURE][BAND_LOLO].latching = true;
    sc->wear.breakage_enabled = false;
    sc->wear.crash_enabled = false;

    alarm_state_t st;
    alarm_init(&st);
    uint32_t t = 0;

    /* A broken loop reads 0 bar, which is below the LoLo limit. Without
     * fault isolation this would be indistinguishable from a real process
     * fault and would trip a latching critical alarm. */
    alarm_input_t in = {0};
    in.value[QTY_CURRENT]  = 20.0f;
    in.value[QTY_PRESSURE] = 0.0f;
    in.value[QTY_RPM]      = 1200.0f;
    in.quantity_faulted[QTY_PRESSURE] = true;

    alarm_update(&st, sc, &in, true, t);
    t += MS(500);
    alarm_update(&st, sc, &in, true, t);

    CHECK(!st.bands[QTY_PRESSURE][BAND_LOLO].active,
          "a faulted sensor must NOT trip its process band");
    CHECK(!st.any_alarm, "a wiring fault must not raise a process alarm");
    CHECK(st.severity == SEV_DIAG,
          "severity should be SEV_DIAG, got %s", severity_str(st.severity));

    /* The same reading with a healthy sensor is a real alarm. */
    alarm_init(&st);
    t = 0;
    in.quantity_faulted[QTY_PRESSURE] = false;
    alarm_update(&st, sc, &in, true, t);
    t += MS(500);
    alarm_update(&st, sc, &in, true, t);

    CHECK(st.bands[QTY_PRESSURE][BAND_LOLO].active,
          "the same value with a healthy sensor IS a real alarm");
    CHECK(st.any_alarm, "and must raise any_alarm");
}

static void test_breakage_and_crash(void)
{
    section("breakage and crash detection");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    spindle_cfg_t *sc = &cfg.spindle[0];

    for (int q = 0; q < QTY_COUNT; q++)
        for (int b = 0; b < BAND_COUNT; b++)
            sc->bands[q][b].enabled = false;

    sc->wear.breakage_enabled = true;
    sc->wear.breakage_drop_pct = 40;
    sc->wear.breakage_window_ms = 100;
    sc->wear.crash_enabled = true;
    sc->wear.crash_rise_pct = 60;
    sc->wear.crash_window_ms = 100;

    alarm_state_t st;
    alarm_init(&st);
    uint32_t t = 0;

    /* Establish a steady cut at 20 A. */
    for (int i = 0; i < 8; i++) {
        feed(&st, sc, 20.0f, true, t);
        t += MS(50);
    }
    CHECK(!st.breakage, "a steady cut must not look like a breakage");
    CHECK(!st.crash, "a steady cut must not look like a crash");

    /* Tool snaps: load collapses to 5 A, a 75% drop. */
    t += MS(50);
    feed(&st, sc, 5.0f, true, t);
    CHECK(st.breakage, "a 75%% drop must be detected as breakage");
    CHECK(st.severity == SEV_BREAKAGE, "severity should be SEV_BREAKAGE, got %s",
          severity_str(st.severity));
    CHECK(st.any_alarm, "breakage must raise any_alarm");

    /* Now a crash from a steady cut. */
    alarm_init(&st);
    t = 0;
    for (int i = 0; i < 8; i++) {
        feed(&st, sc, 20.0f, true, t);
        t += MS(50);
    }
    t += MS(50);
    feed(&st, sc, 40.0f, true, t);
    CHECK(st.crash, "a 100%% rise must be detected as a crash");
    CHECK(st.severity == SEV_CRASH, "crash must outrank everything, got %s",
          severity_str(st.severity));

    /* A modest variation within normal cutting must stay quiet. This is
     * the false-positive case that matters most. */
    alarm_init(&st);
    t = 0;
    for (int i = 0; i < 8; i++) {
        feed(&st, sc, 20.0f, true, t);
        t += MS(50);
    }
    t += MS(50);
    feed(&st, sc, 24.0f, true, t);   /* +20%, below the 60% threshold */
    CHECK(!st.crash, "a 20%% rise must not be a crash");
    t += MS(50);
    feed(&st, sc, 16.0f, true, t);   /* -20%, below the 40% threshold */
    CHECK(!st.breakage, "a 20%% drop must not be a breakage");

    /* Disarmed: transients must be ignored entirely. */
    alarm_init(&st);
    t = 0;
    for (int i = 0; i < 8; i++) {
        feed(&st, sc, 20.0f, false, t);
        t += MS(50);
    }
    t += MS(50);
    feed(&st, sc, 2.0f, false, t);
    CHECK(!st.breakage, "transients must be ignored while disarmed");

    /* Near-zero reference must not produce a divide-by-noise detection.
     * An idle spindle drifting between 0.1 and 0.3 A is a 200% change. */
    alarm_init(&st);
    t = 0;
    for (int i = 0; i < 8; i++) {
        feed(&st, sc, 0.1f, true, t);
        t += MS(50);
    }
    t += MS(50);
    feed(&st, sc, 0.3f, true, t);
    CHECK(!st.crash, "noise on a near-zero reference must not trip a crash");
}

static void test_wear_trend(void)
{
    section("wear trend");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    wear_cfg_t *w = &cfg.spindle[0].wear;
    w->trend_enabled = true;
    w->trend_cycles = 5;

    alarm_state_t st;
    alarm_init(&st);

    /* Steadily rising cycle means: wear. */
    float mean = 20.0f;
    for (int i = 0; i < 6; i++) {
        alarm_on_cycle_end(&st, w, mean);
        mean *= 1.05f;
    }
    CHECK(st.trend, "five consecutive rising cycles must raise the trend");

    /* One good cycle resets the run. */
    alarm_init(&st);
    alarm_on_cycle_end(&st, w, 20.0f);
    alarm_on_cycle_end(&st, w, 21.0f);
    alarm_on_cycle_end(&st, w, 22.0f);
    alarm_on_cycle_end(&st, w, 20.0f);   /* drop */
    alarm_on_cycle_end(&st, w, 21.0f);
    alarm_on_cycle_end(&st, w, 22.0f);
    CHECK(!st.trend, "a non-rising cycle must reset the run");

    /* Noise below the 2% deadband must not count as rising. */
    alarm_init(&st);
    for (int i = 0; i < 8; i++) {
        alarm_on_cycle_end(&st, w, 20.0f + (i % 2) * 0.1f);
    }
    CHECK(!st.trend, "sub-2%% jitter must not accumulate into a trend");
}

/* ============================================================
 * uint32_t millisecond wrap boundary (FIRMWARE_DESIGN_SPEC.md §4.1, §7)
 *
 * The whole point of these three: run the SAME delta sequences as their
 * already-covered non-wrapped counterparts above, just starting the clock
 * a few hundred milliseconds before UINT32_MAX instead of at 0, so every
 * elapsed-time comparison in band_eval()/spindle_sm_update()/
 * hist_reference() is forced to straddle the 49.7-day rollover. Wrap-safe
 * unsigned subtraction is invariant under adding a constant to both
 * operands — including one that wraps — so if the wrap-safe idiom is
 * implemented correctly, these three produce EXACTLY the same pass/fail
 * outcomes as their non-wrapped twins. A mismatch here means a comparison
 * somewhere reverted to the unsafe `now >= since + delay` form.
 * ============================================================ */

static void test_alarm_wrap_boundary(void)
{
    section("alarm delays across the uint32 ms wrap");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    spindle_cfg_t *sc = &cfg.spindle[0];

    for (int q = 0; q < QTY_COUNT; q++)
        for (int b = 0; b < BAND_COUNT; b++)
            sc->bands[q][b].enabled = false;

    /* Same band setup as test_alarm_delays_and_hysteresis(). */
    sc->bands[QTY_CURRENT][BAND_HI].enabled = true;
    sc->bands[QTY_CURRENT][BAND_HI].limit = 35.0f;
    sc->bands[QTY_CURRENT][BAND_HI].hysteresis = 0.5f;
    sc->bands[QTY_CURRENT][BAND_HI].on_delay_ms = 500;
    sc->bands[QTY_CURRENT][BAND_HI].off_delay_ms = 1000;
    sc->bands[QTY_CURRENT][BAND_HI].latching = false;
    sc->wear.breakage_enabled = false;
    sc->wear.crash_enabled = false;

    alarm_state_t st;
    alarm_init(&st);
    /* 300 ms before the rollover — the sequence below crosses it partway
     * through the on-delay wait. */
    uint32_t t = (uint32_t)0xFFFFFFFFu - 300u;

    const band_state_t *hi = &st.bands[QTY_CURRENT][BAND_HI];

    feed(&st, sc, 20.0f, true, t);
    CHECK(!hi->active, "below limit must be inactive (pre-wrap)");

    t += MS(100); feed(&st, sc, 40.0f, true, t);
    CHECK(hi->raw, "raw comparison should be true immediately (pre-wrap)");
    CHECK(!hi->active, "on-delay must suppress the immediate assert (pre-wrap)");

    /* This step crosses UINT32_MAX. Cumulative time above the limit is
     * 300 ms — still under the 500 ms on-delay. */
    t += MS(300); feed(&st, sc, 40.0f, true, t);
    CHECK(!hi->active, "still within the 500 ms on-delay at 300 ms (straddles the wrap)");

    /* Cumulative 500 ms — delay elapses entirely on the post-wrap side. */
    t += MS(200); feed(&st, sc, 40.0f, true, t);
    CHECK(hi->active, "must assert once the on-delay has elapsed (post-wrap)");
    CHECK(CLOSE(hi->value_at_trip, 40.0f, 0.01f),
          "value at trip recorded correctly across the wrap, got %.2f", hi->value_at_trip);

    /* Off-delay, entirely post-wrap. */
    t += MS(100); feed(&st, sc, 30.0f, true, t);
    CHECK(!hi->raw, "30 A is below limit-hysteresis, raw must go false (post-wrap)");
    CHECK(hi->active, "off-delay must hold it active initially (post-wrap)");

    t += MS(500); feed(&st, sc, 30.0f, true, t);
    CHECK(hi->active, "still within the 1000 ms off-delay at 500 ms (post-wrap)");

    t += MS(600); feed(&st, sc, 30.0f, true, t);
    CHECK(!hi->active, "must clear once the off-delay has elapsed (post-wrap)");
}

static void test_spindle_sm_wrap_boundary(void)
{
    section("spindle state machine across the uint32 ms wrap");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    const spindle_sm_cfg_t *sc = &cfg.spindle[0].sm;
    /* defaults: start 50 rpm, settle 500 ms, idle 3 A, cut 6 A, inhibit 500 ms */

    spindle_sm_t sm;
    uint32_t t = (uint32_t)0xFFFFFFFFu - 250u;
    spindle_sm_init(&sm, t);

    /* Same delta sequence as test_spindle_state_machine()'s opening run,
     * offset so the SPIN_UP -> IDLE settle window straddles the wrap. */
    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 1.0f, 40.0f, t);
    CHECK(sm.state == SPINDLE_SPIN_UP, "turning -> SPIN_UP, got %s",
          spindle_state_str(sm.state));

    t += MS(200);
    spindle_sm_update(&sm, sc, 1200.0f, false, 45.0f, 40.0f, t);
    CHECK(sm.state == SPINDLE_SPIN_UP, "still SPIN_UP before settle (straddles the wrap)");
    CHECK(!sm.monitoring_armed, "must NOT be armed during spin-up surge");

    /* Cumulative 600 ms since SPIN_UP entry — settle (500 ms) elapses on
     * the post-wrap side. */
    t += MS(400);
    spindle_sm_update(&sm, sc, 1200.0f, false, 1.5f, 40.0f, t);
    CHECK(sm.state == SPINDLE_IDLE, "after settle -> IDLE across the wrap, got %s",
          spindle_state_str(sm.state));

    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 4.0f, 40.0f, t);
    CHECK(sm.state == SPINDLE_IDLE, "4 A stays IDLE (post-wrap)");

    t += MS(100);
    spindle_sm_update(&sm, sc, 1200.0f, false, 20.0f, 60.0f, t);
    CHECK(sm.state == SPINDLE_CUTTING, "above cut detect -> CUTTING (post-wrap)");
    CHECK(!sm.monitoring_armed, "inhibit window must suppress arming on entry (post-wrap)");

    t += MS(600);
    spindle_sm_update(&sm, sc, 1200.0f, false, 20.0f, 60.0f, t);
    CHECK(sm.monitoring_armed, "armed once the inhibit window has passed (post-wrap)");
}

static void test_transient_wrap_boundary(void)
{
    section("breakage detection across the uint32 ms wrap");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    spindle_cfg_t *sc = &cfg.spindle[0];

    for (int q = 0; q < QTY_COUNT; q++)
        for (int b = 0; b < BAND_COUNT; b++)
            sc->bands[q][b].enabled = false;

    /* Same setup as test_breakage_and_crash()'s breakage case — exercises
     * hist_reference()'s window comparison, the third and last elapsed-time
     * comparison site in the shared modules. */
    sc->wear.breakage_enabled = true;
    sc->wear.breakage_drop_pct = 40;
    sc->wear.breakage_window_ms = 100;
    sc->wear.crash_enabled = false;

    alarm_state_t st;
    alarm_init(&st);
    uint32_t t = (uint32_t)0xFFFFFFFFu - 220u;

    for (int i = 0; i < 8; i++) {
        feed(&st, sc, 20.0f, true, t);
        t += MS(50);
    }
    CHECK(!st.breakage, "a steady cut must not look like a breakage (straddles the wrap)");

    t += MS(50);
    feed(&st, sc, 5.0f, true, t);
    CHECK(st.breakage, "a 75%% drop must still be detected as breakage (post-wrap)");
}

/* ============================================================
 * Output mapping — RETIRED.
 *
 * test_output_mapping() tested do_map.c, which tested the ESP32's own
 * configurable DO-source matrix (do_cfg_t/do_source_t/DO_SRC_*). In V2
 * the ESP32 drives no physical output at all — each SMU's four outputs
 * are a fixed mapping in SMU firmware, not a configurable source matrix
 * on the master (FIRMWARE_DESIGN_SPEC.md §3.4, §2.3). do_map.c/.h and
 * the do_cfg_t, do_source_t, DO_SRC_* constants, and dout[] schema field
 * are deleted along with it. The SMU-side fixed mapping gets its own
 * coverage when that firmware exists, not here.
 * ============================================================ */

/* ============================================================
 * SMU wire protocol (shared/smu_proto.h)
 *
 * The struct-size _Static_asserts inside smu_proto.h are the primary
 * check here and fire at compile time — this is, notably, the FIRST time
 * that header has ever been compiled by anything; until now its five
 * assertions were only verified by hand-computing field sizes. If this
 * file builds at all, every one of them has already passed. What follows
 * is the runtime behaviour a compile-time assert cannot cover: the actual
 * CRC algorithm against the standard test vector, and that windows/CRC
 * behave the way the frame format assumes.
 * ============================================================ */

static void test_smu_proto(void)
{
    section("smu wire protocol");

    /* CRC-16/CCITT-FALSE standard check value — the canonical vector every
     * implementation of this exact variant (poly 0x1021, init 0xFFFF, no
     * reflection) is verified against. */
    CHECK(smu_crc16("123456789", 9) == 0x29B1,
          "CRC16/CCITT-FALSE('123456789') must equal the standard check value 0x29B1");

    CHECK(smu_crc16("", 0) == 0xFFFF,
          "CRC of an empty buffer must equal the init value (no bits processed)");

    /* Determinism and sensitivity: same input twice must match; a single
     * flipped byte must not. A CRC that failed either of these would defeat
     * the entire point of putting one on the wire. */
    const char *msg = "spindle configuration payload";
    uint16_t c1 = smu_crc16(msg, strlen(msg));
    uint16_t c2 = smu_crc16(msg, strlen(msg));
    CHECK(c1 == c2, "CRC must be deterministic across repeated calls");

    char tampered[64];
    strcpy(tampered, msg);
    tampered[5] ^= 0x01;   /* flip one bit */
    CHECK(smu_crc16(tampered, strlen(msg)) != c1,
          "a single flipped bit anywhere in the payload must change the CRC");

    /* SMU_CRC_PAYLOAD_LEN must exclude exactly the trailing crc16 field —
     * get this wrong and either the CRC covers itself (never validates) or
     * misses part of the payload (never catches corruption in that part). */
    CHECK(SMU_CRC_PAYLOAD_LEN(smu_telemetry_t) == sizeof(smu_telemetry_t) - 2,
          "telemetry CRC payload length must be struct size minus 2");
    CHECK(SMU_CRC_PAYLOAD_LEN(smu_command_t) == sizeof(smu_command_t) - 2,
          "command CRC payload length must be struct size minus 2");
    CHECK(SMU_CRC_PAYLOAD_LEN(smu_config_t) == sizeof(smu_config_t) - 2,
          "config CRC payload length must be struct size minus 2");

    /* Round-trip: compute a CRC over a real struct's payload span the way
     * both ends of the link actually will, confirm it is reproducible, and
     * confirm corrupting one byte inside that exact span is caught. This
     * is the CRC used in anger, not just the abstract algorithm above. */
    smu_command_t cmd = {0};
    cmd.opcode = SMU_CMD_ACKNOWLEDGE;
    cmd.nonce  = 0xDEADBEEFu;
    cmd.crc16  = smu_crc16(&cmd, SMU_CRC_PAYLOAD_LEN(smu_command_t));
    uint16_t recomputed = smu_crc16(&cmd, SMU_CRC_PAYLOAD_LEN(smu_command_t));
    CHECK(recomputed == cmd.crc16,
          "recomputing over an unmodified command frame must match the stored CRC");

    smu_command_t corrupt = cmd;
    corrupt.arg8 ^= 0xFF;
    CHECK(smu_crc16(&corrupt, SMU_CRC_PAYLOAD_LEN(smu_command_t)) != cmd.crc16,
          "corrupting a command frame after sealing must be caught by the CRC");

    /* Register windows must not overlap and must be big enough for the
     * blocks assigned to them — the _Static_asserts in smu_proto.h already
     * enforce this at compile time; these are the same invariants restated
     * as runtime checks so a future change to the constants themselves
     * (not just the structs) is still caught here even if someone edits
     * around the static asserts. */
    CHECK(SMU_REG_IDENT + SMU_REG_WINDOW <= SMU_REG_TELEMETRY,
          "ident window must not reach into the telemetry window");
    CHECK(SMU_REG_TELEMETRY + SMU_REG_WINDOW <= SMU_REG_COMMAND,
          "telemetry window must not reach into the command window");
    CHECK(SMU_REG_COMMAND + SMU_REG_WINDOW <= SMU_REG_CONFIG,
          "command window must not reach into the config window");
    CHECK(sizeof(smu_config_t) <= SMU_REG_CONFIG_WINDOW,
          "config struct must fit inside its own window");

    /* I2C addresses must differ — see smu_proto.h's rationale: distinct
     * addresses per spindle turn a swapped daughter card into a bring-up
     * ACK failure instead of silently swapped telemetry. */
    CHECK(SMU_I2C_ADDR_SPINDLE_1 != SMU_I2C_ADDR_SPINDLE_2,
          "the two SMU I2C addresses must be distinct");
    CHECK(SMU_I2C_ADDR_SPINDLE_1 >= 0x08 && SMU_I2C_ADDR_SPINDLE_1 <= 0x77,
          "SMU 1 address must avoid the I2C reserved ranges");
    CHECK(SMU_I2C_ADDR_SPINDLE_2 >= 0x08 && SMU_I2C_ADDR_SPINDLE_2 <= 0x77,
          "SMU 2 address must avoid the I2C reserved ranges");
}

/* ============================================================
 * spindle_cfg_t <-> smu_config_t pack/unpack (smu_pack.c)
 *
 * This is the one place spindle_config.h and smu_proto.h meet, and it is
 * hand-written field-by-field rather than generated — exactly the kind
 * of code where a single mis-mapped field is silent (both sides compile,
 * both sides look reasonable, and the SMU quietly runs on the wrong
 * cut-detect threshold or the wrong wear baseline forever). Round-trip
 * through pack then unpack and check the result equals the original.
 * ============================================================ */

static void test_smu_pack(void)
{
    section("smu config pack/unpack");

    spindle_cfg_t src;
    spindle_cfg_set_defaults(&src, 1);   /* index 1 -> exercises "Spindle 2" */
    src.machine_running_enabled = false; /* deliberately non-default, to
                                           * make sure the flag bit is
                                           * actually threaded, not just
                                           * coincidentally right */
    src.pressure.mode = PRESSURE_INPUT_0_10V;
    src.min_pulse_ms = 1500;

    smu_config_t wire;
    smu_config_pack(&src, /*spindle_index=*/1, /*schema_version=*/42,
                    /*mains_hz=*/60, &wire);

    /* The pack function must produce a self-consistent, verifiable frame:
     * recomputing the CRC over the payload it just wrote must match what
     * it stored. If this fails, nothing downstream matters — a frame
     * that doesn't validate against its own CRC would never even reach
     * the unpack step on a real link. */
    uint16_t recomputed = smu_crc16(&wire, SMU_CRC_PAYLOAD_LEN(smu_config_t));
    CHECK(recomputed == wire.crc16,
          "packed frame must validate against its own CRC");

    CHECK(wire.schema_version == 42, "schema_version threaded through as given");
    CHECK(wire.spindle_index == 1, "spindle_index threaded through as given");
    CHECK(wire.mains_hz == 60, "mains_hz threaded through as given");
    CHECK((wire.flags & SMU_CFG_SPINDLE_ENABLED) != 0,
          "enabled=true must set SMU_CFG_SPINDLE_ENABLED");
    CHECK((wire.flags & SMU_CFG_MACHINE_RUN_ENABLED) == 0,
          "machine_running_enabled=false must clear SMU_CFG_MACHINE_RUN_ENABLED");
    CHECK(wire.pressure_mode == PRESSURE_INPUT_0_10V,
          "pressure mode threaded through");
    CHECK(wire.min_pulse_ms == 1500, "min_pulse_ms threaded through");
    CHECK(CLOSE(wire.adaptive_k_warn, src.wear.adaptive_k_warn, 0.0001f),
          "adaptive_k_warn survives the pack");

    spindle_cfg_t back;
    uint8_t spindle_index_out = 0xFF, mains_hz_out = 0;
    smu_config_unpack(&wire, &back, &spindle_index_out, &mains_hz_out);

    CHECK(spindle_index_out == 1, "unpack recovers spindle_index via out-param");
    CHECK(mains_hz_out == 60, "unpack recovers mains_hz via out-param");

    CHECK(back.enabled == src.enabled, "enabled round-trips");
    CHECK(back.machine_running_enabled == src.machine_running_enabled,
          "machine_running_enabled round-trips");
    CHECK(back.min_pulse_ms == src.min_pulse_ms, "min_pulse_ms round-trips");

    CHECK(CLOSE(back.current.ct_primary_amps, src.current.ct_primary_amps, 0.0001f),
          "current.ct_primary_amps round-trips");
    CHECK(CLOSE(back.current.gain_correction, src.current.gain_correction, 0.0001f),
          "current.gain_correction round-trips");
    CHECK(back.current.rms_burst_samples == src.current.rms_burst_samples,
          "current.rms_burst_samples round-trips");

    CHECK(back.pressure.mode == src.pressure.mode, "pressure.mode round-trips");
    CHECK(CLOSE(back.pressure.sensor_max, src.pressure.sensor_max, 0.0001f),
          "pressure.sensor_max round-trips");

    CHECK(back.rpm.pulses_per_rev == src.rpm.pulses_per_rev,
          "rpm.pulses_per_rev round-trips");

    CHECK(CLOSE(back.sm.cut_detect_current_a, src.sm.cut_detect_current_a, 0.0001f),
          "sm.cut_detect_current_a round-trips");

    CHECK(back.wear.breakage_enabled == src.wear.breakage_enabled,
          "wear.breakage_enabled round-trips");
    CHECK(back.wear.baseline_learn_cycles == src.wear.baseline_learn_cycles,
          "wear.baseline_learn_cycles round-trips");
    CHECK(CLOSE(back.wear.adaptive_k_alarm, src.wear.adaptive_k_alarm, 0.0001f),
          "wear.adaptive_k_alarm round-trips");

    /* Every band, every quantity — this is exactly the 3x4x6 = 72-field
     * surface the web UI redesign (FIRMWARE_DESIGN_SPEC.md §5.6) exists
     * to make editable; it deserves the same rigour on the wire path. */
    for (int q = 0; q < QTY_COUNT; q++) {
        for (int b = 0; b < BAND_COUNT; b++) {
            const band_cfg_t *o = &src.bands[q][b];
            const band_cfg_t *r = &back.bands[q][b];
            CHECK(r->enabled == o->enabled && r->latching == o->latching &&
                  CLOSE(r->limit, o->limit, 0.0001f) &&
                  CLOSE(r->hysteresis, o->hysteresis, 0.0001f) &&
                  r->on_delay_ms == o->on_delay_ms &&
                  r->off_delay_ms == o->off_delay_ms,
                  "band[%d][%d] round-trips completely", q, b);
        }
    }

    /* And the flip side of the enabled-flag check above. */
    src.machine_running_enabled = true;
    smu_config_pack(&src, 0, 8, 50, &wire);
    CHECK((wire.flags & SMU_CFG_MACHINE_RUN_ENABLED) != 0,
          "machine_running_enabled=true must set SMU_CFG_MACHINE_RUN_ENABLED");
}

/* ============================================================
 * smu_link.c — protocol logic against a fake in-memory transport
 *
 * No I2C, no ESP-IDF: a fake_smu_t is just three byte buffers standing
 * in for the SMU's register windows, wired to smu_link_t through
 * smu_transport_t. This is precisely the seam smu_link.c was written
 * around, and it is what makes the staleness/CRC/link-state logic —
 * the genuinely safety-adjacent part of this file, since getting it
 * wrong means a stale reading gets treated as live — testable at all
 * without hardware.
 * ============================================================ */

typedef struct {
    uint8_t  ident[sizeof(smu_ident_t)];
    uint8_t  telem[sizeof(smu_telemetry_t)];
    uint8_t  cfg[sizeof(smu_config_t)];
    bool     fail_reads;
    bool     fail_writes;
    bool     ignore_commands;   /* simulate an SMU that never processes a command */
    uint32_t delay_calls;
} fake_smu_t;

static esp_err_t fake_read(void *ctx, uint16_t reg, void *out, size_t len)
{
    fake_smu_t *f = (fake_smu_t *)ctx;
    if (f->fail_reads) return ESP_FAIL;
    if (reg == SMU_REG_IDENT      && len <= sizeof(f->ident)) { memcpy(out, f->ident, len); return ESP_OK; }
    if (reg == SMU_REG_TELEMETRY  && len <= sizeof(f->telem)) { memcpy(out, f->telem, len); return ESP_OK; }
    if (reg == SMU_REG_CONFIG     && len <= sizeof(f->cfg))   { memcpy(out, f->cfg,   len); return ESP_OK; }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t fake_write(void *ctx, uint16_t reg, const void *data, size_t len)
{
    fake_smu_t *f = (fake_smu_t *)ctx;
    if (f->fail_writes) return ESP_FAIL;
    if (reg == SMU_REG_CONFIG && len <= sizeof(f->cfg)) {
        memcpy(f->cfg, data, len);
        return ESP_OK;
    }
    if (reg == SMU_REG_COMMAND && len == sizeof(smu_command_t)) {
        if (f->ignore_commands) return ESP_OK;   /* accepted on the bus, never acted on */
        smu_command_t cmd;
        memcpy(&cmd, data, len);
        smu_ident_t id;
        memcpy(&id, f->ident, sizeof(id));
        id.last_cmd_nonce  = cmd.nonce;
        id.last_cmd_result = SMU_RESULT_OK;
        memcpy(f->ident, &id, sizeof(id));
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static void fake_delay(void *ctx, uint32_t us)
{
    fake_smu_t *f = (fake_smu_t *)ctx;
    f->delay_calls++;
    (void)us;   /* no real sleep in a host test */
}

static smu_transport_t fake_transport(fake_smu_t *f)
{
    smu_transport_t t = { .read = fake_read, .write = fake_write,
                          .delay_us = fake_delay, .ctx = f };
    return t;
}

static void fake_set_ident(fake_smu_t *f)
{
    smu_ident_t id;
    memset(&id, 0, sizeof(id));
    id.magic         = SMU_PROTO_MAGIC;
    id.proto_version = SMU_PROTO_VERSION;
    memcpy(f->ident, &id, sizeof(id));
}

static void fake_set_telemetry(fake_smu_t *f, uint16_t seq, float current_a)
{
    smu_telemetry_t t;
    memset(&t, 0, sizeof(t));
    t.seq       = seq;
    t.current_a = current_a;
    t.crc16     = smu_crc16(&t, SMU_CRC_PAYLOAD_LEN(smu_telemetry_t));
    memcpy(f->telem, &t, sizeof(t));
}

static void test_smu_link(void)
{
    section("smu_link against a fake transport");

    fake_smu_t f;
    memset(&f, 0, sizeof(f));
    fake_set_ident(&f);
    fake_set_telemetry(&f, 1, 10.0f);

    smu_transport_t tr = fake_transport(&f);
    smu_link_t link;
    smu_link_init(&link, &tr, 0);

    CHECK(link.state == SMU_LINK_DOWN, "a fresh link starts DOWN");
    CHECK(!smu_link_is_fresh(&link, 0, 1000000),
          "a link with no frame yet is never fresh");

    /* --- First successful poll ---------------------------------------- */
    smu_link_poll(&link, 1000);
    CHECK(link.state == SMU_LINK_LIVE, "first valid frame brings the link LIVE");
    CHECK(link.have_good, "have_good set after the first valid frame");
    CHECK(CLOSE(link.last_good.current_a, 10.0f, 0.001f),
          "telemetry current_a carried through");
    CHECK(smu_link_is_fresh(&link, 1000, 100000),
          "freshly-polled data is fresh");
    CHECK(!smu_link_is_fresh(&link, 1000 + 200000, 100000),
          "data older than max_age_us is not fresh");

    /* --- Stalled SMU: CRC valid, seq unchanged ------------------------ */
    smu_link_poll(&link, 2000);   /* fake still has seq=1 */
    CHECK(link.state == SMU_LINK_DEGRADED,
          "a frozen sequence number degrades a LIVE link, not crash it outright");
    CHECK(link.consecutive_failures == 1, "one stall counts as one failure");
    CHECK(link.last_good.current_a == 10.0f,
          "last_good must NOT be overwritten by a stalled/rejected frame");

    /* --- Recovery ------------------------------------------------------ */
    fake_set_telemetry(&f, 2, 11.5f);
    smu_link_poll(&link, 3000);
    CHECK(link.state == SMU_LINK_LIVE, "a fresh sequence number recovers to LIVE");
    CHECK(link.consecutive_failures == 0, "recovery resets the failure streak");
    CHECK(CLOSE(link.last_good.current_a, 11.5f, 0.001f),
          "last_good updates on recovery");

    /* --- Corrupted CRC -------------------------------------------------- */
    fake_set_telemetry(&f, 3, 12.0f);
    f.telem[10] ^= 0xFF;   /* corrupt a payload byte after the CRC was computed */
    smu_link_poll(&link, 4000);
    CHECK(link.state == SMU_LINK_DEGRADED, "a bad CRC degrades a LIVE link");
    CHECK(link.crc_error_count == 1, "CRC failures are counted separately from timeouts");
    CHECK(CLOSE(link.last_good.current_a, 11.5f, 0.001f),
          "last_good must NOT be overwritten by a CRC-invalid frame");

    /* --- Sustained failure -> DOWN --------------------------------------
     * Already at 1 consecutive failure from the CRC corruption above;
     * SMU_LINK_DOWN_THRESHOLD more transport failures should cross into
     * DOWN. */
    f.fail_reads = true;
    for (int i = 0; i < SMU_LINK_DOWN_THRESHOLD; i++) {
        smu_link_poll(&link, 5000 + (int64_t)i * 1000);
    }
    CHECK(link.state == SMU_LINK_DOWN,
          "enough consecutive failures declares the link DOWN, not merely degraded");
    CHECK(link.timeout_count >= (uint32_t)SMU_LINK_DOWN_THRESHOLD,
          "each failed transport read counts as a timeout");

    /* --- Wrong magic is treated as invalid, not trusted ----------------- */
    fake_smu_t f2;
    memset(&f2, 0, sizeof(f2));
    fake_set_ident(&f2);
    smu_ident_t bad_id;
    memcpy(&bad_id, f2.ident, sizeof(bad_id));
    bad_id.magic = 0xDEAD;
    memcpy(f2.ident, &bad_id, sizeof(bad_id));
    fake_set_telemetry(&f2, 1, 99.0f);

    smu_transport_t tr2 = fake_transport(&f2);
    smu_link_t link2;
    smu_link_init(&link2, &tr2, 1);
    smu_link_poll(&link2, 1000);
    CHECK(link2.state != SMU_LINK_LIVE,
          "a frame with the wrong magic must never bring the link LIVE");
    CHECK(!link2.have_good, "a wrong-magic frame must not populate last_good");

    /* --- Config push: happy path ---------------------------------------- */
    fake_smu_t f3;
    memset(&f3, 0, sizeof(f3));
    fake_set_ident(&f3);
    fake_set_telemetry(&f3, 1, 5.0f);
    smu_transport_t tr3 = fake_transport(&f3);
    smu_link_t link3;
    smu_link_init(&link3, &tr3, 0);

    spindle_cfg_t cfg;
    spindle_cfg_set_defaults(&cfg, 0);

    uint8_t result = 0xFF;
    esp_err_t err = smu_link_push_config(&link3, &cfg, 8, 50, 500000, &result);
    CHECK(err == ESP_OK, "push_config succeeds when the fake SMU echoes the nonce immediately");
    CHECK(result == SMU_RESULT_OK, "result_out carries the SMU's reported result");
    CHECK(f3.delay_calls == 0,
          "an immediate echo must not need to wait at all");

    /* Config actually landed in the fake's register file, packed
     * correctly — spot-check one field through the whole path. */
    smu_config_t landed;
    memcpy(&landed, f3.cfg, sizeof(landed));
    CHECK(CLOSE(landed.cut_detect_current_a, cfg.sm.cut_detect_current_a, 0.001f),
          "pushed config actually reached the transport's config register");

    /* --- Config push: SMU never processes the command ------------------- */
    fake_smu_t f4;
    memset(&f4, 0, sizeof(f4));
    fake_set_ident(&f4);
    fake_set_telemetry(&f4, 1, 5.0f);
    f4.ignore_commands = true;
    smu_transport_t tr4 = fake_transport(&f4);
    smu_link_t link4;
    smu_link_init(&link4, &tr4, 0);

    err = smu_link_push_config(&link4, &cfg, 8, 50, 100000, &result);
    CHECK(err == ESP_ERR_TIMEOUT,
          "push_config times out if the SMU never echoes the command nonce");
    CHECK(f4.delay_calls > 0, "a timing-out push_config must actually have waited/retried");

    /* --- send_command_wait: the generic version calib_autozero() uses --- */
    fake_smu_t f5;
    memset(&f5, 0, sizeof(f5));
    fake_set_ident(&f5);
    fake_set_telemetry(&f5, 1, 5.0f);
    smu_transport_t tr5 = fake_transport(&f5);
    smu_link_t link5;
    smu_link_init(&link5, &tr5, 0);

    result = 0xFF;
    err = smu_link_send_command_wait(&link5, SMU_CMD_AUTOZERO, 0, 0, 500000, &result);
    CHECK(err == ESP_OK, "send_command_wait succeeds when the fake SMU echoes immediately");
    CHECK(result == SMU_RESULT_OK, "send_command_wait reports the SMU's result code");

    fake_smu_t f6;
    memset(&f6, 0, sizeof(f6));
    fake_set_ident(&f6);
    fake_set_telemetry(&f6, 1, 5.0f);
    f6.ignore_commands = true;
    smu_transport_t tr6 = fake_transport(&f6);
    smu_link_t link6;
    smu_link_init(&link6, &tr6, 0);

    err = smu_link_send_command_wait(&link6, SMU_CMD_AUTOZERO, 0, 0, 100000, &result);
    CHECK(err == ESP_ERR_TIMEOUT,
          "send_command_wait times out the same way push_config does when ignored");
}

/* ============================================================
 * Job templates (main/job_template.c)
 *
 * The first test below is the load-bearing one for the whole job-template
 * feature. Everything else here is ordinary coverage; that one is a guard
 * against a specific silent failure: if someone later adds a machine field to
 * job_profile_t or writes one in job_profile_apply(), calibration starts
 * travelling between machines and NOTHING looks broken -- the device runs, the
 * diagnostics pass, and every reading is quietly wrong by the ratio between
 * the two machines' correction factors. Do not delete it.
 * ============================================================ */

/* Fills a spindle config with distinctive, non-default machine values, so a
 * leak from one config into another is obvious rather than coincidentally
 * equal to whatever the default happened to be. */
static void make_machine(spindle_cfg_t *s, int index, float ct_amps,
                         const char *unit, float p_min, float p_max)
{
    spindle_cfg_set_defaults(s, index);
    s->current.ct_primary_amps   = ct_amps;
    s->current.ct_secondary_ma   = 1000.0f + (float)index;
    s->current.gain_correction   = 0.5f + (float)index * 0.01f;
    s->current.zero_offset_v     = 1.6f + (float)index * 0.01f;
    s->current.noload_cutoff_a   = 0.3f + (float)index * 0.01f;
    s->current.rms_burst_samples = (uint16_t)(128 + index);
    s->pressure.sensor_min       = p_min;
    s->pressure.sensor_max       = p_max;
    s->pressure.gain_correction  = 1.1f + (float)index * 0.01f;
    s->pressure.offset_correction = 0.2f + (float)index * 0.01f;
    s->pressure.mode             = (index == 0) ? PRESSURE_INPUT_4_20MA
                                                : PRESSURE_INPUT_0_10V;
    snprintf(s->pressure.unit, CFG_UNIT_LEN, "%s", unit);
    s->rpm.pulses_per_rev        = (uint16_t)(1 + index);
    s->rpm.glitch_filter_ns      = (uint16_t)(50000 + index);
    s->rpm.zero_timeout_ms       = (uint16_t)(500 + index);
    snprintf(s->name, CFG_NAME_LEN, "Machine spindle %d", index);
}

static void test_job_template_machine_fields_survive(void)
{
    section("job template: machine fields survive an apply");

    /* Two DIFFERENT machines: different CT rating, different sensor span,
     * different calibration, different everything machine-shaped. */
    spindle_cfg_t source, dest, dest_before;
    make_machine(&source, 0, 30.0f, "bar", 0.0f, 250.0f);
    make_machine(&dest,   1, 100.0f, "bar", 0.0f, 400.0f);

    /* Give the source distinctive JOB values so we can prove they DID move. */
    source.sm.cut_detect_current_a = 7.25f;
    source.sm.idle_current_a       = 2.5f;
    source.sm.start_rpm            = 123.0f;
    source.min_pulse_ms            = 2500;
    source.machine_running_enabled = true;
    source.wear.breakage_drop_pct  = 44;
    source.wear.trend_cycles       = 9;
    source.bands[QTY_CURRENT][BAND_HI].enabled = true;
    source.bands[QTY_CURRENT][BAND_HI].limit   = 21.5f;
    source.bands[QTY_PRESSURE][BAND_LO].enabled = true;
    source.bands[QTY_PRESSURE][BAND_LO].limit   = 11.5f;

    dest_before = dest;   /* byte-for-byte snapshot before the apply */

    job_profile_t prof;
    job_profile_extract(&source, &prof);
    job_profile_apply(&prof, &dest);

    /* --- The job half MUST have moved ---------------------------------- */
    CHECK(CLOSE(dest.sm.cut_detect_current_a, 7.25f, 0.001f),
          "cut-detect current is job data and must transfer");
    CHECK(CLOSE(dest.sm.start_rpm, 123.0f, 0.001f),
          "start RPM is job data and must transfer");
    CHECK(dest.min_pulse_ms == 2500, "min_pulse_ms is job data and must transfer");
    CHECK(dest.wear.breakage_drop_pct == 44, "wear settings are job data");
    CHECK(dest.wear.trend_cycles == 9, "wear trend cycles are job data");
    CHECK(dest.bands[QTY_CURRENT][BAND_HI].enabled &&
          CLOSE(dest.bands[QTY_CURRENT][BAND_HI].limit, 21.5f, 0.001f),
          "threshold bands are job data and must transfer");
    CHECK(CLOSE(dest.bands[QTY_PRESSURE][BAND_LO].limit, 11.5f, 0.001f),
          "all quantities' bands transfer, not just current");

    /* --- The machine half MUST NOT have moved --------------------------
     * Each of these, if it leaked, produces a device that runs perfectly and
     * reads wrong forever. */
    CHECK(CLOSE(dest.current.ct_primary_amps, dest_before.current.ct_primary_amps, 0.0001f),
          "CT rating must NOT be overwritten by a job template");
    CHECK(CLOSE(dest.current.ct_secondary_ma, dest_before.current.ct_secondary_ma, 0.0001f),
          "CT secondary must NOT be overwritten");
    CHECK(CLOSE(dest.current.gain_correction, dest_before.current.gain_correction, 0.0001f),
          "current gain correction must NOT be overwritten -- this is THE bug this guards");
    CHECK(CLOSE(dest.current.zero_offset_v, dest_before.current.zero_offset_v, 0.0001f),
          "auto-zero tare must NOT be overwritten");
    CHECK(CLOSE(dest.current.noload_cutoff_a, dest_before.current.noload_cutoff_a, 0.0001f),
          "no-load cutoff must NOT be overwritten");
    CHECK(dest.current.rms_burst_samples == dest_before.current.rms_burst_samples,
          "RMS burst samples must NOT be overwritten");
    CHECK(CLOSE(dest.pressure.sensor_min, dest_before.pressure.sensor_min, 0.0001f),
          "pressure sensor min must NOT be overwritten");
    CHECK(CLOSE(dest.pressure.sensor_max, dest_before.pressure.sensor_max, 0.0001f),
          "pressure sensor max must NOT be overwritten");
    CHECK(CLOSE(dest.pressure.gain_correction, dest_before.pressure.gain_correction, 0.0001f),
          "pressure gain correction must NOT be overwritten");
    CHECK(CLOSE(dest.pressure.offset_correction, dest_before.pressure.offset_correction, 0.0001f),
          "pressure offset correction must NOT be overwritten");
    CHECK(dest.pressure.mode == dest_before.pressure.mode,
          "pressure input mode must NOT be overwritten");
    CHECK(strcmp(dest.pressure.unit, dest_before.pressure.unit) == 0,
          "pressure unit must NOT be overwritten");
    CHECK(dest.rpm.pulses_per_rev == dest_before.rpm.pulses_per_rev,
          "pulses-per-rev must NOT be overwritten");
    CHECK(dest.rpm.glitch_filter_ns == dest_before.rpm.glitch_filter_ns,
          "glitch filter must NOT be overwritten");
    CHECK(dest.rpm.zero_timeout_ms == dest_before.rpm.zero_timeout_ms,
          "zero timeout must NOT be overwritten");
    CHECK(strcmp(dest.name, dest_before.name) == 0,
          "the spindle's own name must NOT be overwritten by a job template");
}

static void test_job_check_unit_mismatch(void)
{
    section("job template: pressure unit mismatch is a hard refusal");

    spindle_cfg_t source, dest;
    make_machine(&source, 0, 30.0f, "bar", 0.0f, 250.0f);
    make_machine(&dest,   1, 30.0f, "psi", 0.0f, 3600.0f);

    job_profile_t prof;
    job_context_t ctx;
    job_profile_extract(&source, &prof);
    job_context_extract(&source, &ctx);

    job_warnings_t warn;
    job_check_result_t r = job_check(&prof, &ctx, &dest, 8, 8, &warn);
    CHECK(r == JOB_ERR_UNIT_MISMATCH,
          "a bar template on a psi machine must be refused, not warned -- the "
          "error is in the permissive direction");

    /* Same unit on both sides is fine. */
    spindle_cfg_t dest_bar;
    make_machine(&dest_bar, 1, 30.0f, "bar", 0.0f, 250.0f);
    r = job_check(&prof, &ctx, &dest_bar, 8, 8, &warn);
    CHECK(r == JOB_OK, "matching units pass");

    /* A same-machine spindle-to-spindle copy passes NULL context and must not
     * trip the unit check. */
    r = job_check(&prof, NULL, &dest, 8, 8, &warn);
    CHECK(r == JOB_OK, "a NULL context (same-machine copy) skips the unit check");
}

static void test_job_check_schema_version(void)
{
    section("job template: schema version gate");

    spindle_cfg_t s;
    make_machine(&s, 0, 30.0f, "bar", 0.0f, 250.0f);

    job_profile_t prof;
    job_context_t ctx;
    job_profile_extract(&s, &prof);
    job_context_extract(&s, &ctx);

    job_warnings_t warn;
    CHECK(job_check(&prof, &ctx, &s, 9, 8, &warn) == JOB_ERR_SCHEMA_NEWER,
          "a template from newer firmware must be refused");
    CHECK(job_check(&prof, &ctx, &s, 8, 8, &warn) == JOB_OK,
          "same schema version is accepted");
    CHECK(job_check(&prof, &ctx, &s, 7, 8, &warn) == JOB_OK,
          "an older template is accepted (fields it lacks keep their defaults)");
}

static void test_job_check_reachability(void)
{
    section("job template: unreachable band warnings");

    spindle_cfg_t source, dest;
    make_machine(&source, 0, 100.0f, "bar", 0.0f, 400.0f);  /* big machine   */
    make_machine(&dest,   1, 30.0f,  "bar", 0.0f, 250.0f);  /* small machine */

    /* Disable everything, then enable exactly the bands under test, so the
     * warning list contains only what this test put there. */
    for (int q = 0; q < QTY_COUNT; q++)
        for (int b = 0; b < BAND_COUNT; b++)
            source.bands[q][b].enabled = false;

    /* 45 A high band on a 30 A CT: can never be reached. */
    source.bands[QTY_CURRENT][BAND_HIHI].enabled = true;
    source.bands[QTY_CURRENT][BAND_HIHI].limit   = 45.0f;

    /* 300 bar high band on a 250 bar sensor: can never be reached. */
    source.bands[QTY_PRESSURE][BAND_HI].enabled = true;
    source.bands[QTY_PRESSURE][BAND_HI].limit   = 300.0f;

    /* 300 bar LOW band on a 250 bar sensor: pressure is always below it, so
     * it trips permanently -- a different problem from "never trips", and
     * worth telling the operator apart. */
    source.bands[QTY_PRESSURE][BAND_LO].enabled = true;
    source.bands[QTY_PRESSURE][BAND_LO].limit   = 300.0f;

    job_profile_t prof;
    job_context_t ctx;
    job_profile_extract(&source, &prof);
    job_context_extract(&source, &ctx);

    job_warnings_t warn;
    job_check_result_t r = job_check(&prof, &ctx, &dest, 8, 8, &warn);

    CHECK(r == JOB_OK,
          "unreachable bands warn but do NOT block -- the product decision is "
          "warn-and-apply, made safe by refusing to apply at all while cutting");
    CHECK(warn.count == 3, "expected 3 warnings, got %u", warn.count);

    bool saw_current_never = false, saw_pressure_never = false, saw_pressure_always = false;
    for (int i = 0; i < warn.count; i++) {
        if (warn.item[i].qty == QTY_CURRENT && warn.item[i].band == BAND_HIHI &&
            warn.item[i].kind == JOB_WARN_NEVER_TRIPS) saw_current_never = true;
        if (warn.item[i].qty == QTY_PRESSURE && warn.item[i].band == BAND_HI &&
            warn.item[i].kind == JOB_WARN_NEVER_TRIPS) saw_pressure_never = true;
        if (warn.item[i].qty == QTY_PRESSURE && warn.item[i].band == BAND_LO &&
            warn.item[i].kind == JOB_WARN_ALWAYS_TRIPS) saw_pressure_always = true;
    }
    CHECK(saw_current_never, "45 A band on a 30 A CT must warn that it never trips");
    CHECK(saw_pressure_never, "300 bar high band on a 250 bar sensor must warn");
    CHECK(saw_pressure_always, "300 bar LOW band on a 250 bar sensor trips constantly");

    /* The same template applied back onto its own (larger) machine is clean. */
    r = job_check(&prof, &ctx, &source, 8, 8, &warn);
    CHECK(r == JOB_OK && warn.count == 0,
          "reachable bands produce no warnings, got %u", warn.count);

    /* A DISABLED band with a wild value must not warn -- the UI already
     * labels those "ignored", and warning about them would train operators
     * to dismiss the warning banner. */
    for (int q = 0; q < QTY_COUNT; q++)
        for (int b = 0; b < BAND_COUNT; b++)
            source.bands[q][b].enabled = false;
    source.bands[QTY_CURRENT][BAND_HIHI].limit = 9999.0f;
    job_profile_extract(&source, &prof);
    r = job_check(&prof, &ctx, &dest, 8, 8, &warn);
    CHECK(r == JOB_OK && warn.count == 0,
          "a disabled band with an impossible limit must not warn, got %u", warn.count);
}

/* ============================================================
 * main
 * ============================================================ */

int main(void)
{
    printf("CNC Tool Monitor — host logic tests\n");
    printf("===================================\n");

    test_pressure_scaling();
    test_current_scaling();
    test_rpm_scaling();
    test_config_validation();
    test_spindle_state_machine();
    test_alarm_delays_and_hysteresis();
    test_alarm_arming_and_latching();
    test_sensor_fault_isolation();
    test_breakage_and_crash();
    test_wear_trend();
    test_alarm_wrap_boundary();
    test_spindle_sm_wrap_boundary();
    test_transient_wrap_boundary();
    test_smu_proto();
    test_smu_pack();
    test_smu_link();
    test_job_template_machine_fields_survive();
    test_job_check_unit_mismatch();
    test_job_check_schema_version();
    test_job_check_reachability();

    printf("\n===================================\n");
    printf("passed: %d   failed: %d\n", g_pass, g_fail);

    return g_fail ? 1 : 0;
}
