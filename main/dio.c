/*
 * dio.c
 */

#include "dio.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board.h"

static const char *TAG = "dio";

static const gpio_num_t k_out_pins[NUM_DIGITAL_OUT] = {
    PIN_DO0, PIN_DO1, PIN_DO2, PIN_DO3
};
static const gpio_num_t k_in_pins[NUM_DIGITAL_IN] = {
    PIN_DI0, PIN_DI1, PIN_DI2, PIN_DI3
};

typedef struct {
    bool     requested;      /* what the alarm engine wants          */
    bool     applied;        /* what is actually on the pin          */
    bool     invert;
    uint16_t min_pulse_ms;
    int64_t  asserted_at_us; /* when `applied` last went true        */
    bool     forced;
    bool     force_value;
} out_state_t;

static out_state_t s_out[NUM_DIGITAL_OUT];
static bool        s_outputs_enabled;

/* ============================================================
 * Internal
 * ============================================================ */

static void write_pin(uint8_t i, bool active)
{
    bool level = active ? (DO_ACTIVE_LEVEL != 0) : (DO_ACTIVE_LEVEL == 0);
    if (s_out[i].invert) level = !level;
    gpio_set_level(k_out_pins[i], level ? 1 : 0);
}

/* ============================================================
 * Init
 * ============================================================ */

esp_err_t dio_init(const app_config_t *cfg)
{
    memset(s_out, 0, sizeof(s_out));
    s_outputs_enabled = false;

    for (int i = 0; i < NUM_DIGITAL_OUT; i++) {
        s_out[i].invert       = cfg->dout[i].invert;
        s_out[i].min_pulse_ms = cfg->dout[i].min_pulse_ms;

        /* Set the level before switching the pin to an output. Doing it in
         * the other order lets the pin drive whatever was last in the
         * output register — which after a watchdog reset is whatever the
         * previous run left there. */
        write_pin(i, false);

        gpio_config_t oc = {
            .pin_bit_mask = 1ULL << k_out_pins[i],
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&oc);
        if (err != ESP_OK) return err;

        write_pin(i, false);
    }

    uint64_t in_mask = 0;
    for (int i = 0; i < NUM_DIGITAL_IN; i++) {
        in_mask |= 1ULL << k_in_pins[i];
    }

    /* DI0/DI1 are GPIO34/35, which are input-only and have no internal
     * pull resistors at all; the request below is a no-op for them and
     * the board must supply external pull-ups (see board.h). It is still
     * worth asking, because DI2/DI3 are ordinary pins and benefit. */
    gpio_config_t ic = {
        .pin_bit_mask = in_mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&ic);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "%d outputs (held inactive), %d inputs configured",
             NUM_DIGITAL_OUT, NUM_DIGITAL_IN);
    return ESP_OK;
}

void dio_enable_outputs(void)
{
    if (s_outputs_enabled) return;
    s_outputs_enabled = true;
    ESP_LOGI(TAG, "outputs released to the alarm engine");

    /* Apply whatever the engine has already asked for during the blackout,
     * notably the system-healthy output, which is inverted and therefore
     * needs an explicit write to reach its energised idle state. */
    for (int i = 0; i < NUM_DIGITAL_OUT; i++) {
        if (s_out[i].requested != s_out[i].applied) {
            s_out[i].applied = s_out[i].requested;
            if (s_out[i].applied) s_out[i].asserted_at_us = esp_timer_get_time();
            write_pin(i, s_out[i].applied);
        }
    }
}

/* ============================================================
 * Output control
 * ============================================================ */

void dio_set(uint8_t index, bool active)
{
    if (index >= NUM_DIGITAL_OUT) return;
    out_state_t *o = &s_out[index];

    if (o->forced) return;

    o->requested = active;

    if (!s_outputs_enabled) return;

    if (active && !o->applied) {
        o->applied = true;
        o->asserted_at_us = esp_timer_get_time();
        write_pin(index, true);
        return;
    }

    /* Clearing is handled by dio_service() so the minimum pulse width can
     * be honoured without blocking the caller. */
}

void dio_service(void)
{
    if (!s_outputs_enabled) return;

    int64_t now = esp_timer_get_time();

    for (int i = 0; i < NUM_DIGITAL_OUT; i++) {
        out_state_t *o = &s_out[i];
        if (o->forced) continue;

        if (!o->requested && o->applied) {
            int64_t held_ms = (now - o->asserted_at_us) / 1000;
            if (held_ms >= (int64_t)o->min_pulse_ms) {
                o->applied = false;
                write_pin(i, false);
            }
            /* else: keep it asserted a little longer so the PLC scan
             * cannot step over the event */
        } else if (o->requested && !o->applied) {
            /* Can happen if the request arrived while outputs were held. */
            o->applied = true;
            o->asserted_at_us = now;
            write_pin(i, true);
        }
    }
}

bool dio_get(uint8_t index)
{
    if (index >= NUM_DIGITAL_OUT) return false;
    return s_out[index].forced ? s_out[index].force_value : s_out[index].applied;
}

/* ============================================================
 * Inputs
 * ============================================================ */

bool dio_read_input(uint8_t index)
{
    if (index >= NUM_DIGITAL_IN) return false;
    int level = gpio_get_level(k_in_pins[index]);
    return (level == DI_ACTIVE_LEVEL);
}

/* ============================================================
 * Forced mode for self-test
 * ============================================================ */

void dio_force(uint8_t index, bool active, bool forced)
{
    if (index >= NUM_DIGITAL_OUT) return;

    s_out[index].forced      = forced;
    s_out[index].force_value = active;

    if (forced) {
        ESP_LOGW(TAG, "DO%u FORCED %s — alarm engine overridden",
                 index, active ? "ON" : "OFF");
        write_pin(index, active);
        s_out[index].applied = active;
        if (active) s_out[index].asserted_at_us = esp_timer_get_time();
    } else {
        ESP_LOGW(TAG, "DO%u force released", index);
        s_out[index].applied = s_out[index].requested;
        write_pin(index, s_out[index].applied);
    }
}

bool dio_is_forced(uint8_t index)
{
    if (index >= NUM_DIGITAL_OUT) return false;
    return s_out[index].forced;
}

void dio_clear_all_forces(void)
{
    for (int i = 0; i < NUM_DIGITAL_OUT; i++) {
        if (s_out[i].forced) dio_force((uint8_t)i, false, false);
    }
}
