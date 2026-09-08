/*
 * analog.c — ADS1115 acquisition for current and pressure.
 *
 * ============================================================
 * READ THIS BEFORE TRUSTING THE CURRENT READING
 * ============================================================
 *
 * The ADS1115 has one delta-sigma core multiplexed across four inputs,
 * with a maximum aggregate throughput of 860 SPS. This board feeds the CT
 * burden voltage straight into it, so the firmware has to reconstruct an
 * RMS value from a sample stream that is barely faster than the waveform
 * it is measuring:
 *
 *     860 SPS / 50 Hz = 17.2 samples per mains cycle   (best case, one
 *                                                       channel dedicated)
 *
 * Seventeen samples per cycle is not enough for a coherent single-cycle
 * RMS. What saves the measurement is averaging over many cycles: the
 * sampling is incoherent with the line frequency, so the phase of the
 * sample train walks through the waveform and the error from the partial
 * cycle at the end of the window shrinks roughly as 1/cycles.
 *
 *     burst   window    cycles    approx RMS error on a clean sine
 *     -----   ------    ------    -------------------------------
 *      32     37 ms      1.9      ~8 %
 *      64     74 ms      3.7      ~4 %
 *     128    149 ms      7.4      ~2 %     <- default
 *     256    298 ms     14.9      ~1 %
 *
 * Two consequences the rest of the system has to live with:
 *
 *  1. Current updates arrive every ~150 ms per channel, and with two
 *     channels sharing the ADC the effective per-spindle rate is ~3 Hz.
 *     That is fine for threshold monitoring against NFR-1's 200 ms budget
 *     only because the alarm task evaluates on arrival rather than on a
 *     fixed tick.
 *
 *  2. Breakage detection (TW-R8) specifies a 100 ms window. One burst is
 *     longer than that, so sub-burst transients are invisible. Breakage
 *     detection on this hardware operates burst-to-burst, giving a real
 *     detection window nearer 300-400 ms. This is a genuine capability
 *     gap, not a tuning problem, and it is why SRS HW-D1 recommends an
 *     external RMS-to-DC front end.
 *
 * The numbers above assume a clean sinusoid. A VFD-driven spindle is not
 * clean, and harmonics above the ~430 Hz Nyquist limit of an 860 SPS
 * stream alias straight down into the reading. Expect field accuracy to
 * be worse than the table, and treat this implementation as a bring-up
 * and bench-testing path rather than a production measurement.
 */

#include "analog.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

#include "ads1115.h"
#include "board.h"

static const char *TAG = "analog";

/* Operating range. At 3V3 the input cannot legally exceed VDD, so the
 * +/-4.096 V and +/-6.144 V settings are unusable however tempting their
 * headroom looks. */
#define OPERATING_FSR   ADS_FSR_2048
#define BURST_SPS       ADS_SPS_860
#define PRESSURE_SPS    ADS_SPS_128   /* slow signal; the extra averaging
                                       * of a lower data rate is free
                                       * noise rejection */

/* Minimum time between bus-recovery attempts once the ADC has been marked
 * unhealthy — see analog_try_recover(). A wedged bus is not worth
 * re-testing on every single read call: that's needless bus traffic, and
 * if the cause is still active (a WiFi TX burst, in the field case this
 * was written for) it is a retry storm that cannot succeed yet anyway. */
#define RECOVERY_MIN_INTERVAL_US   (2 * 1000 * 1000)

static i2c_master_bus_handle_t s_bus;
static ads1115_t              *s_adc;
static SemaphoreHandle_t       s_adc_mutex;
static bool                    s_healthy;
static int64_t                 s_last_recovery_attempt_us;

/* Per-spindle measured DC bias, refreshed by auto-zero. Seeded at init
 * from the stored calibration (current.zero_offset_v) so a tare performed
 * during commissioning survives a power cycle; falls back to the nominal
 * from board.h for a device that has never been zeroed. */
static float s_bias_v[NUM_SPINDLES] = { CT_BIAS_VOLTS, CT_BIAS_VOLTS };

static const uint8_t k_current_ch[NUM_SPINDLES]  = {
    ADS_CH_CURRENT_S1, ADS_CH_CURRENT_S2
};
static const uint8_t k_pressure_ch[NUM_SPINDLES] = {
    ADS_CH_PRESSURE_S1, ADS_CH_PRESSURE_S2
};

/* ============================================================
 * Init
 * ============================================================ */

esp_err_t analog_init(const app_config_t *cfg)
{
    s_adc_mutex = xSemaphoreCreateMutex();
    if (!s_adc_mutex) return ESP_ERR_NO_MEM;

    /* Seeded negative (not 0) so a fault in the first RECOVERY_MIN_INTERVAL_US
     * of uptime is not mistaken for "we just tried" and skipped — exactly
     * the window the bus lockup that motivated this code showed up in
     * (~1.1 s after boot, while WiFi was still bringing up the AP). */
    s_last_recovery_attempt_us = -RECOVERY_MIN_INTERVAL_US;

    /* Restore the stored zero reference. A bias of zero means the field has
     * never been written (a fresh config), so keep the board nominal rather
     * than trusting a value that would make every reading wrong. */
    if (cfg) {
        for (int i = 0; i < NUM_SPINDLES; i++) {
            float stored = cfg->spindle[i].current.zero_offset_v;
            if (stored > 0.0f) s_bias_v[i] = stored;
        }
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_PORT_NUM,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = ads1115_create(s_bus, ADS1115_I2C_ADDR, &s_adc);
    if (err != ESP_OK) return err;

    err = ads1115_probe(s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 not responding at 0x%02X", ADS1115_I2C_ADDR);
        s_healthy = false;
        return err;
    }

    s_healthy = true;
    ESP_LOGI(TAG, "analogue front end ready");
    return ESP_OK;
}

bool analog_is_healthy(void)
{
    return s_healthy;
}

/* ============================================================
 * Recovery
 *
 * Field build: an I2C bus lockup was observed ~1.1 s after boot, timed
 * right as WiFi finished bringing up the fallback AP (DHCP server up,
 * then "I2C bus is still busy but software timeout detected" followed by
 * GPIO 21/22 "not usable, maybe conflict with others" as the driver's own
 * internal stuck-bus handling tripped over pins it already owned). Most
 * likely cause: WiFi radio TX transients coupling onto a bus that only has
 * the ESP32's weak internal pull-ups at 400 kHz (see board.h) — a hardware
 * fix (external 2.2-4.7k pull-ups) is the real cure, but the bug this
 * function closes is a SEPARATE one: before this, s_healthy was set true
 * exactly once, at init, and nothing ever attempted to clear a fault —
 * one transient glitch anywhere in a boot latched adc:FAIL for the rest
 * of it, regardless of whether the bus recovered on its own a second
 * later.
 * ============================================================ */

/* Attempt to bring the ADC back into service after an I2C fault.
 *
 * i2c_master_bus_reset() is the ESP-IDF-native recovery primitive: it
 * toggles SCL to force a slave that's holding SDA low to release it, then
 * issues a STOP. That is the only thing that can actually unstick a wedged
 * bus. A successful reset only proves the wires are free, not that the
 * ADS1115 is behaving — a genuinely dead or unplugged chip would also let
 * the wires float free — so health is restored only after a full
 * ads1115_probe() succeeds too, exactly the same bar analog_init() uses.
 *
 * Rate-limited via RECOVERY_MIN_INTERVAL_US so a bus that stays down does
 * not get hammered by every read call across both spindles, and does not
 * fill the log with an attempt per call. Safe to call whenever s_healthy
 * is false; a no-op (after the rate-limit check) otherwise-adjacent
 * callers don't need to reason about. */
static void analog_try_recover(void)
{
    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);

    int64_t now = esp_timer_get_time();
    if (now - s_last_recovery_attempt_us < RECOVERY_MIN_INTERVAL_US) {
        xSemaphoreGive(s_adc_mutex);
        return;
    }
    s_last_recovery_attempt_us = now;

    esp_err_t err = i2c_master_bus_reset(s_bus);
    if (err != ESP_OK) {
        xSemaphoreGive(s_adc_mutex);
        ESP_LOGW(TAG, "I2C bus recovery failed: %s", esp_err_to_name(err));
        return;
    }

    err = ads1115_probe(s_adc);
    xSemaphoreGive(s_adc_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus reset ok, but ADS1115 still not responding: %s",
                 esp_err_to_name(err));
        return;
    }

    s_healthy = true;
    ESP_LOGI(TAG, "ADS1115 recovered — I2C bus back in service");
}

/* ============================================================
 * Current — burst RMS
 * ============================================================ */

esp_err_t analog_read_current(uint8_t spindle, const current_cfg_t *cfg,
                              current_reading_t *out)
{
    if (spindle >= NUM_SPINDLES || !cfg || !out) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    /* A wedged bus does not clear itself. Try recovery before spending a
     * whole burst window (~150 ms) on a transaction that would just fail
     * again — see analog_try_recover(). */
    if (!s_healthy) analog_try_recover();
    if (!s_healthy) {
        out->status = SENSOR_OPEN;
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t n = cfg->rms_burst_samples;
    const uint8_t  ch = k_current_ch[spindle];
    const uint32_t period_us = ads1115_period_us(BURST_SPS);

    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);

    esp_err_t err = ads1115_start_continuous(s_adc, ch, OPERATING_FSR, BURST_SPS);
    if (err != ESP_OK) {
        xSemaphoreGive(s_adc_mutex);
        s_healthy = false;
        return err;
    }

    /* Accumulate sum and sum-of-squares in one pass. The DC bias is
     * removed afterwards using the burst's own mean rather than the stored
     * auto-zero value, because the mean of a full burst is a better
     * estimate of the present bias than a calibration taken minutes ago —
     * it tracks temperature drift for free. The stored bias is kept as a
     * sanity check. */
    double sum = 0.0, sum_sq = 0.0;
    uint32_t taken = 0;
    int64_t t0 = esp_timer_get_time();

    for (uint16_t i = 0; i < n; i++) {
        int16_t raw;
        err = ads1115_read_conversion(s_adc, &raw);
        if (err != ESP_OK) break;

        float v = ads1115_to_volts(raw, OPERATING_FSR);
        sum    += v;
        sum_sq += (double)v * (double)v;
        taken++;

        /* Pace to just over one conversion period so each read lands on a
         * fresh conversion. Reading faster would duplicate samples, which
         * biases the RMS toward whatever the waveform happened to be doing
         * at that instant. */
        esp_rom_delay_us(period_us + 50);
    }

    int64_t t1 = esp_timer_get_time();
    ads1115_stop_continuous(s_adc);
    xSemaphoreGive(s_adc_mutex);

    if (err != ESP_OK || taken < 8) {
        s_healthy = false;
        out->status = SENSOR_OPEN;
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_STATE;
    }

    double mean = sum / (double)taken;
    double mean_sq = sum_sq / (double)taken;

    /* Variance about the mean is the AC power; its square root is the AC
     * RMS with the DC bias already removed. Guard the tiny negative that
     * floating-point cancellation can produce on a silent channel. */
    double variance = mean_sq - mean * mean;
    if (variance < 0.0) variance = 0.0;

    float vrms = (float)sqrt(variance);

    /* Cross-check the burst mean against the stored zero. A large
     * discrepancy means either the bias network has drifted badly or the
     * CT has saturated hard enough to skew the mean — both worth flagging
     * rather than silently scaling. */
    float bias_error = fabsf((float)mean - s_bias_v[spindle]);
    sensor_status_t status = SENSOR_OK;
    if (bias_error > 0.25f) {
        status = SENSOR_OVERRANGE;
        ESP_LOGW(TAG, "spindle %u CT bias drifted: %.3f V vs %.3f V stored",
                 spindle, (float)mean, s_bias_v[spindle]);
    }

    out->burden_vrms = vrms;
    out->amps        = current_scale(cfg, vrms);
    out->status      = status;
    out->window_us   = (uint32_t)(t1 - t0);

    return ESP_OK;
}

esp_err_t analog_autozero(uint8_t spindle, float *measured_bias_v)
{
    if (spindle >= NUM_SPINDLES) return ESP_ERR_INVALID_ARG;

    if (!s_healthy) analog_try_recover();
    if (!s_healthy) return ESP_ERR_INVALID_STATE;

    const uint8_t ch = k_current_ch[spindle];
    const uint32_t period_us = ads1115_period_us(BURST_SPS);
    const int n = 256;   /* long window: the point is a stable mean */

    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);

    esp_err_t err = ads1115_start_continuous(s_adc, ch, OPERATING_FSR, BURST_SPS);
    if (err == ESP_OK) {
        double sum = 0.0;
        int taken = 0;
        for (int i = 0; i < n; i++) {
            int16_t raw;
            if (ads1115_read_conversion(s_adc, &raw) != ESP_OK) break;
            sum += ads1115_to_volts(raw, OPERATING_FSR);
            taken++;
            esp_rom_delay_us(period_us + 50);
        }
        ads1115_stop_continuous(s_adc);

        if (taken >= n / 2) {
            float bias = (float)(sum / taken);
            /* Refuse a bias that is nowhere near the design value — that
             * is a wiring or component fault, and storing it would bake
             * the fault into the calibration. */
            if (fabsf(bias - CT_BIAS_VOLTS) > 0.3f) {
                ESP_LOGE(TAG, "auto-zero rejected: measured %.3f V, expected ~%.3f V",
                         bias, (float)CT_BIAS_VOLTS);
                err = ESP_ERR_INVALID_RESPONSE;
            } else {
                s_bias_v[spindle] = bias;
                if (measured_bias_v) *measured_bias_v = bias;
                ESP_LOGI(TAG, "spindle %u auto-zero: %.4f V", spindle, bias);
            }
        } else {
            err = ESP_ERR_TIMEOUT;
        }
    }

    xSemaphoreGive(s_adc_mutex);
    return err;
}

/* ============================================================
 * Pressure — single shot
 * ============================================================ */

esp_err_t analog_read_pressure(uint8_t spindle, const pressure_cfg_t *cfg,
                               pressure_reading_t *out)
{
    if (spindle >= NUM_SPINDLES || !cfg || !out) return ESP_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (!s_healthy) analog_try_recover();
    if (!s_healthy) {
        out->status = SENSOR_OPEN;
        return ESP_ERR_INVALID_STATE;
    }

    int16_t raw;
    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);
    esp_err_t err = ads1115_read_single(s_adc, k_pressure_ch[spindle],
                                        ADS_FSR_4096, PRESSURE_SPS, &raw);
    xSemaphoreGive(s_adc_mutex);

    if (err != ESP_OK) {
        s_healthy = false;
        out->status = SENSOR_OPEN;
        return err;
    }

    float volts = ads1115_to_volts(raw, ADS_FSR_4096);

    out->adc_volts = volts;
    out->loop_ma   = pressure_loop_ma(volts);
    out->status    = pressure_scale(cfg, volts, &out->value);

    return ESP_OK;
}

esp_err_t analog_read_raw(uint8_t ads_channel, int16_t *raw)
{
    if (ads_channel > 3 || !raw) return ESP_ERR_INVALID_ARG;

    if (!s_healthy) analog_try_recover();
    if (!s_healthy) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_adc_mutex, portMAX_DELAY);
    esp_err_t err = ads1115_read_single(s_adc, ads_channel,
                                        OPERATING_FSR, PRESSURE_SPS, raw);
    xSemaphoreGive(s_adc_mutex);
    return err;
}
