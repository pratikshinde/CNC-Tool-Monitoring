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
#include "do_map.h"

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

    /* Output mapping. */
    bad = cfg;
    bad.dout[0].spindle = 9;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_DO_SOURCE,
          "output mapped to a nonexistent spindle must be rejected");

    /* A zero quantity_mask would silently never assert — indistinguishable
     * from "working, nothing wrong" — so it is rejected rather than
     * accepted as a quiet no-op output. */
    bad = cfg;
    bad.dout[0].source = DO_SRC_SPINDLE_QUANTITY;
    bad.dout[0].spindle = 0;
    bad.dout[0].quantity_mask = 0;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_DO_SOURCE,
          "a zero quantity_mask must be rejected");

    bad.dout[0].quantity_mask = (uint8_t)(DO_QTY_ALL + 1);
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_ERR_DO_SOURCE,
          "a quantity_mask with bits outside DO_QTY_ALL must be rejected");

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
          "a cutoff at 50% of CT rating must be rejected (would hide real load)");

    bad = cfg;
    bad.spindle[0].current.noload_cutoff_a =
        bad.spindle[0].current.ct_primary_amps * 0.05f;
    app_config_seal(&bad);
    CHECK(app_config_validate(&bad, NULL) == CFG_OK,
          "a cutoff at exactly 5% of CT rating must be accepted");

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

#define MS(x) ((int64_t)(x) * 1000)

static void test_spindle_state_machine(void)
{
    section("spindle state machine");

    app_config_t cfg;
    app_config_set_defaults(&cfg);
    const spindle_sm_cfg_t *sc = &cfg.spindle[0].sm;
    /* defaults: start 50 rpm, settle 500 ms, idle 3 A, cut 6 A, inhibit 500 ms */

    spindle_sm_t sm;
    int64_t t = 0;
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
                 float current, bool armed, int64_t t)
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
    int64_t t = 0;

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
    int64_t t = 0;

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
    int64_t t = 0;

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
    int64_t t = 0;

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
 * Output mapping
 * ============================================================ */

static void test_output_mapping(void)
{
    section("digital output mapping");

    app_config_t cfg;
    app_config_set_defaults(&cfg);

    /* This test exercises do_map_evaluate()'s source types directly rather
     * than relying on whatever app_config_set_defaults() currently ships as
     * the factory dout[] preset — the two have drifted apart before (the
     * factory preset is now DO_SRC_SPINDLE_QUANTITY throughout, per a
     * customer-specific requirement) and coupling this test to that preset
     * would make it fail every time the preset changes for reasons that
     * have nothing to do with do_map.c's correctness. */
    cfg.dout[0] = (do_cfg_t){ .source = DO_SRC_SPINDLE_ALARM,   .spindle = 0, .min_pulse_ms = 500 };
    cfg.dout[1] = (do_cfg_t){ .source = DO_SRC_SPINDLE_ALARM,   .spindle = 1, .min_pulse_ms = 500 };
    cfg.dout[2] = (do_cfg_t){ .source = DO_SRC_SPINDLE_WARNING, .spindle = 0, .min_pulse_ms = 500 };
    cfg.dout[3] = (do_cfg_t){ .source = DO_SRC_SYSTEM_HEALTHY };

    alarm_state_t a0, a1;
    alarm_init(&a0);
    alarm_init(&a1);

    do_map_input_t in = {
        .alarm = { &a0, &a1 },
        .spindle_enabled = { true, true },
        .system_healthy = true,
        .diagnostic_fault = false,
    };

    bool out[NUM_DIGITAL_OUT];

    /* Quiet system: only the healthy output is asserted. */
    do_map_evaluate(&cfg, &in, out);
    CHECK(!out[0], "DO0 (S1 alarm) quiet");
    CHECK(!out[1], "DO1 (S2 alarm) quiet");
    CHECK(!out[2], "DO2 (S1 warning) quiet");
    CHECK(out[3],  "DO3 (system healthy) asserted when healthy");

    /* Spindle 1 alarms. */
    a0.any_alarm = true;
    do_map_evaluate(&cfg, &in, out);
    CHECK(out[0],  "DO0 follows spindle 1 alarm");
    CHECK(!out[1], "DO1 must NOT follow spindle 1 — outputs are independent");

    /* Spindle 2 alarms independently. */
    a0.any_alarm = false;
    a1.any_alarm = true;
    do_map_evaluate(&cfg, &in, out);
    CHECK(!out[0], "DO0 clear");
    CHECK(out[1],  "DO1 follows spindle 2 alarm");

    /* Warning routing. */
    a1.any_alarm = false;
    a0.any_warning = true;
    do_map_evaluate(&cfg, &in, out);
    CHECK(!out[0], "a warning must not assert the ALARM output");
    CHECK(out[2],  "a warning must assert the WARNING output");

    /* System health drops. */
    a0.any_warning = false;
    in.system_healthy = false;
    do_map_evaluate(&cfg, &in, out);
    CHECK(!out[3], "system-healthy output de-asserts on fault");

    /* A disabled spindle must not be able to drive an output. */
    in.system_healthy = true;
    in.spindle_enabled[0] = false;
    a0.any_alarm = true;
    do_map_evaluate(&cfg, &in, out);
    CHECK(!out[0], "a disabled spindle must not drive its output");

    /* ANY_ALARM aggregation. */
    in.spindle_enabled[0] = true;
    app_config_t agg = cfg;
    agg.dout[0].source = DO_SRC_ANY_ALARM;
    a0.any_alarm = false;
    a1.any_alarm = true;
    do_map_evaluate(&agg, &in, out);
    CHECK(out[0], "ANY_ALARM must catch spindle 2");

    a1.any_alarm = false;
    do_map_evaluate(&agg, &in, out);
    CHECK(!out[0], "ANY_ALARM clear when both spindles are quiet");

    /* Diagnostic faults must be separable from process alarms (DG-R3). */
    app_config_t diag = cfg;
    diag.dout[0].source = DO_SRC_DIAG_FAULT;
    in.diagnostic_fault = true;
    do_map_evaluate(&diag, &in, out);
    CHECK(out[0], "diagnostic fault output asserts");
    CHECK(!out[1], "a diagnostic fault must not assert a process alarm output");

    /* --- DO_SRC_SPINDLE_QUANTITY: per-quantity output masks ------------
     * Exercises the customer requirement: current+RPM on one output,
     * pressure alone on another, per spindle. */
    section("digital output mapping — per-quantity masks");

    in.diagnostic_fault = false;
    in.spindle_enabled[0] = in.spindle_enabled[1] = true;
    alarm_init(&a0);
    alarm_init(&a1);
    memset(in.rpm_sensor_suspect, 0, sizeof(in.rpm_sensor_suspect));

    app_config_t qc = cfg;
    qc.dout[0] = (do_cfg_t){
        .source = DO_SRC_SPINDLE_QUANTITY, .spindle = 0,
        .quantity_mask = DO_QTY_CURRENT | DO_QTY_RPM,
    };
    qc.dout[1] = (do_cfg_t){
        .source = DO_SRC_SPINDLE_QUANTITY, .spindle = 0,
        .quantity_mask = DO_QTY_PRESSURE,
    };

    do_map_evaluate(&qc, &in, out);
    CHECK(!out[0] && !out[1], "quiet spindle: both quantity outputs clear");

    /* A current-band violation must reach the current+RPM output, not the
     * pressure-only one. */
    a0.bands[QTY_CURRENT][BAND_HI].active = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(out[0],  "current band active reaches the current+RPM output");
    CHECK(!out[1], "current band active must not reach the pressure-only output");
    a0.bands[QTY_CURRENT][BAND_HI].active = false;

    /* A pressure-band violation is the mirror image. */
    a0.bands[QTY_PRESSURE][BAND_LOLO].active = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(!out[0], "pressure band active must not reach the current+RPM output");
    CHECK(out[1],  "pressure band active reaches the pressure-only output");
    a0.bands[QTY_PRESSURE][BAND_LOLO].active = false;

    /* A latched-but-no-longer-active band must still count — same
     * active-or-latched test alarm.c itself uses for any_alarm/any_warning. */
    a0.bands[QTY_PRESSURE][BAND_LOLO].latched = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(out[1], "a latched pressure band still reaches the pressure output");
    a0.bands[QTY_PRESSURE][BAND_LOLO].latched = false;

    /* Breakage/crash/wear-trend are current-signature conditions and must
     * roll into the current quantity even with no band active. */
    a0.breakage = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(out[0], "breakage reaches the current+RPM output");
    CHECK(!out[1], "breakage must not reach the pressure-only output");
    a0.breakage = false;

    a0.crash_latched = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(out[0], "a latched crash reaches the current+RPM output");
    a0.crash_latched = false;

    a0.trend = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(out[0], "wear trend reaches the current+RPM output");
    a0.trend = false;

    /* A suspect RPM sensor (current flowing, no pulses) is an RPM-side
     * anomaly even though it never sets an RPM band. */
    in.rpm_sensor_suspect[0] = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(out[0],  "a suspect RPM sensor reaches the current+RPM output");
    CHECK(!out[1], "a suspect RPM sensor must not reach the pressure-only output");
    in.rpm_sensor_suspect[0] = false;

    /* Spindle independence still holds for this source too. */
    a1.bands[QTY_CURRENT][BAND_HIHI].active = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(!out[0], "spindle 2's current fault must not reach spindle 1's output");
    a1.bands[QTY_CURRENT][BAND_HIHI].active = false;

    /* A disabled spindle must not be able to drive a quantity output. */
    in.spindle_enabled[0] = false;
    a0.bands[QTY_CURRENT][BAND_HI].active = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(!out[0], "a disabled spindle must not drive its quantity output");
    in.spindle_enabled[0] = true;
    a0.bands[QTY_CURRENT][BAND_HI].active = false;

    /* An empty or out-of-range mask is a config error, caught by
     * app_config_validate() (see test_config_validation) — not exercised
     * here since do_map_evaluate() itself has no validation to do; a zero
     * mask simply never matches any quantity and the output stays clear. */
    qc.dout[0].quantity_mask = 0;
    a0.bands[QTY_CURRENT][BAND_HI].active = true;
    do_map_evaluate(&qc, &in, out);
    CHECK(!out[0], "a zero quantity_mask never asserts");
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
    test_output_mapping();

    printf("\n===================================\n");
    printf("passed: %d   failed: %d\n", g_pass, g_fail);

    return g_fail ? 1 : 0;
}
