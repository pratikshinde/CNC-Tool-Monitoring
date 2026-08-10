/*
 * rpm.c
 *
 * Two estimators, because neither alone covers 10 to 24 000 RPM (AI-R10):
 *
 *   Frequency mode (fast): count pulses over a fixed gate. Resolution is
 *   +/-1 pulse per gate, so at a 100 ms gate and 1 PPR the quantisation is
 *   600 RPM — useless at low speed, excellent at high speed where the
 *   count is large.
 *
 *   Period mode (slow): measure the time between consecutive pulses.
 *   Resolution is set by the timer, which is microseconds here, so it is
 *   effectively exact at low speed. At high speed the interval shrinks
 *   toward the sampling jitter and the reading gets noisy.
 *
 * The crossover is 600 RPM, chosen so that frequency mode always has at
 * least ~10 counts in the gate before it takes over.
 *
 * ----------------------------------------------------------------------
 * A note on the debounce requirement (AI-R11)
 * ----------------------------------------------------------------------
 * The SRS asks for a configurable digital debounce of 0-5 ms. The PCNT
 * glitch filter is clocked from APB at 80 MHz with a 10-bit threshold, so
 * its ceiling is:
 *
 *     1023 / 80 MHz = 12.8 us
 *
 * Three orders of magnitude short of 5 ms. That requirement cannot be met
 * in hardware — but it also should not be: 5 ms of debounce imposes a
 * 200 Hz pulse ceiling, which at 1 PPR caps the measurable speed at
 * 12 000 RPM and at 4 PPR at 3 000 RPM. The SRS figure is wrong for the
 * application, not merely unimplementable. This driver caps the filter at
 * 12 us and reports the clamp; SRS AI-R11 should be amended to match.
 */

#include "rpm.h"

#include <string.h>

#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"
#include "scaling.h"

static const char *TAG = "rpm";

#define PCNT_HIGH_LIMIT     32767
#define PCNT_LOW_LIMIT     (-1)

/* Above this, trust the pulse count over the gate; below, trust the
 * interval between pulses. */
#define MODE_CROSSOVER_RPM  600.0f

/* Hardware ceiling of the PCNT glitch filter, from the APB clock. */
#define GLITCH_FILTER_MAX_NS  12750

typedef struct {
    pcnt_unit_handle_t    unit;
    pcnt_channel_handle_t chan;

    int      last_count;
    int64_t  last_sample_us;

    /* Timestamp of the sample at which the count last changed. Used both
     * for the stopped timeout and as the basis of the period estimate. */
    int64_t  last_edge_us;
    int      pulses_at_last_edge;

    uint32_t total_pulses;
    float    last_rpm;
} rpm_channel_t;

static rpm_channel_t s_ch[NUM_RPM_CH];
static const gpio_num_t k_pins[NUM_RPM_CH] = { PIN_DI0, PIN_DI1 };

/* ============================================================
 * Init
 * ============================================================ */

static esp_err_t apply_filter(rpm_channel_t *c, uint16_t requested_ns)
{
    uint32_t ns = requested_ns;
    if (ns > GLITCH_FILTER_MAX_NS) {
        ESP_LOGW(TAG, "glitch filter %u ns clamped to hardware max %u ns",
                 (unsigned)ns, (unsigned)GLITCH_FILTER_MAX_NS);
        ns = GLITCH_FILTER_MAX_NS;
    }

    if (ns == 0) {
        return pcnt_unit_set_glitch_filter(c->unit, NULL);
    }

    pcnt_glitch_filter_config_t f = { .max_glitch_ns = ns };
    return pcnt_unit_set_glitch_filter(c->unit, &f);
}

esp_err_t rpm_init(const app_config_t *cfg)
{
    memset(s_ch, 0, sizeof(s_ch));

    for (int i = 0; i < NUM_RPM_CH; i++) {
        rpm_channel_t *c = &s_ch[i];

        pcnt_unit_config_t unit_cfg = {
            .high_limit = PCNT_HIGH_LIMIT,
            .low_limit  = PCNT_LOW_LIMIT,
        };
        ESP_RETURN_ON_ERROR(pcnt_new_unit(&unit_cfg, &c->unit), TAG,
                            "pcnt_new_unit %d", i);

        pcnt_chan_config_t chan_cfg = {
            .edge_gpio_num  = k_pins[i],
            .level_gpio_num = -1,        /* no direction control */
        };
        ESP_RETURN_ON_ERROR(pcnt_new_channel(c->unit, &chan_cfg, &c->chan), TAG,
                            "pcnt_new_channel %d", i);

        /* The opto stage inverts, so a field pulse appears as a falling
         * edge at the MCU. Count on the falling edge and ignore the rise
         * so one field pulse produces exactly one count. */
        ESP_RETURN_ON_ERROR(
            pcnt_channel_set_edge_action(c->chan,
                                         PCNT_CHANNEL_EDGE_ACTION_HOLD,
                                         PCNT_CHANNEL_EDGE_ACTION_INCREASE),
            TAG, "edge action %d", i);
        ESP_RETURN_ON_ERROR(
            pcnt_channel_set_level_action(c->chan,
                                          PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                          PCNT_CHANNEL_LEVEL_ACTION_KEEP),
            TAG, "level action %d", i);

        ESP_RETURN_ON_ERROR(apply_filter(c, cfg->spindle[i].rpm.glitch_filter_ns),
                            TAG, "glitch filter %d", i);

        ESP_RETURN_ON_ERROR(pcnt_unit_enable(c->unit),  TAG, "enable %d", i);
        ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(c->unit), TAG, "clear %d", i);
        ESP_RETURN_ON_ERROR(pcnt_unit_start(c->unit),   TAG, "start %d", i);

        int64_t now = esp_timer_get_time();
        c->last_sample_us = now;
        c->last_edge_us   = now;
    }

#ifdef DI_NEEDS_EXTERNAL_PULLUP
    ESP_LOGI(TAG, "DI0/DI1 are input-only pins — external pull-ups required");
#endif

    ESP_LOGI(TAG, "%d RPM channels ready", NUM_RPM_CH);
    return ESP_OK;
}

esp_err_t rpm_reconfigure(const app_config_t *cfg)
{
    for (int i = 0; i < NUM_RPM_CH; i++) {
        /* The filter cannot be changed while the unit is running. */
        ESP_RETURN_ON_ERROR(pcnt_unit_stop(s_ch[i].unit),    TAG, "stop %d", i);
        ESP_RETURN_ON_ERROR(pcnt_unit_disable(s_ch[i].unit), TAG, "disable %d", i);
        ESP_RETURN_ON_ERROR(apply_filter(&s_ch[i], cfg->spindle[i].rpm.glitch_filter_ns),
                            TAG, "filter %d", i);
        ESP_RETURN_ON_ERROR(pcnt_unit_enable(s_ch[i].unit),  TAG, "enable %d", i);
        ESP_RETURN_ON_ERROR(pcnt_unit_start(s_ch[i].unit),   TAG, "start %d", i);
    }
    return ESP_OK;
}

/* ============================================================
 * Sampling
 * ============================================================ */

esp_err_t rpm_read(uint8_t channel, const rpm_cfg_t *cfg, rpm_reading_t *out)
{
    if (channel >= NUM_RPM_CH || !cfg || !out) return ESP_ERR_INVALID_ARG;

    rpm_channel_t *c = &s_ch[channel];
    memset(out, 0, sizeof(*out));

    int count = 0;
    ESP_RETURN_ON_ERROR(pcnt_unit_get_count(c->unit, &count), TAG, "get_count");

    int64_t now = esp_timer_get_time();

    /* Handle the counter wrapping at the high limit. */
    int delta = count - c->last_count;
    if (delta < 0) delta += (PCNT_HIGH_LIMIT + 1);

    float gate_ms = (float)(now - c->last_sample_us) / 1000.0f;

    c->total_pulses += (uint32_t)delta;

    if (delta > 0) {
        c->last_edge_us = now;
        c->pulses_at_last_edge = delta;
    }

    /* Stopped: no edges within the timeout (AI-R12). Checked before either
     * estimator so a stationary spindle reports a hard zero rather than an
     * ever-decaying period estimate. */
    int64_t since_edge_ms = (now - c->last_edge_us) / 1000;
    if (since_edge_ms >= (int64_t)cfg->zero_timeout_ms) {
        c->last_count      = count;
        c->last_sample_us  = now;
        c->last_rpm        = 0.0f;
        out->rpm           = 0.0f;
        out->stopped       = true;
        out->pulses_total  = c->total_pulses;
        return ESP_OK;
    }

    float rpm;
    bool period_mode;

    /* Provisional frequency estimate decides which estimator to trust. */
    float freq_rpm = rpm_from_count((unsigned)delta, gate_ms, cfg->pulses_per_rev);

    if (freq_rpm >= MODE_CROSSOVER_RPM) {
        rpm = freq_rpm;
        period_mode = false;
    } else if (delta > 0) {
        /* Period mode: the gate contained `delta` pulses, so the mean
         * interval is the gate divided by the count. With delta == 1 this
         * degenerates to the gate width, which is exactly what we want at
         * very low speed. */
        float interval_us = (gate_ms * 1000.0f) / (float)delta;
        rpm = rpm_from_interval(interval_us, cfg->pulses_per_rev);
        period_mode = true;
    } else {
        /* Turning slowly enough that this gate caught no pulse at all, but
         * not yet timed out. Hold the last value rather than reporting a
         * spurious zero — a momentary zero would drop the state machine
         * out of CUTTING and disarm monitoring mid-cut. */
        rpm = c->last_rpm;
        period_mode = true;
    }

    c->last_count     = count;
    c->last_sample_us = now;
    c->last_rpm       = rpm;

    out->rpm              = rpm;
    out->stopped          = false;
    out->pulses_total     = c->total_pulses;
    out->used_period_mode = period_mode;

    return ESP_OK;
}
