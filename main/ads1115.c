/*
 * ads1115.c
 *
 * Register map:
 *   0x00 conversion (r)   0x01 config (rw)
 *   0x02 lo_thresh (rw)   0x03 hi_thresh (rw)
 *
 * Config register bit layout:
 *   15    OS         1 = start single conversion (w) / 1 = idle (r)
 *   14:12 MUX        100..111 = AIN0..AIN3 single-ended vs GND
 *   11:9  PGA        full-scale range
 *   8     MODE       0 = continuous, 1 = single-shot
 *   7:5   DR         data rate
 *   4     COMP_MODE  0 = traditional
 *   3     COMP_POL   0 = active low
 *   2     COMP_LAT   0 = non-latching
 *   1:0   COMP_QUE   11 = comparator disabled, ALERT pin high-Z
 */

#include "ads1115.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "ads1115";

#define REG_CONVERSION  0x00
#define REG_CONFIG      0x01

#define CFG_OS_SINGLE   (1u << 15)
#define CFG_MODE_SINGLE (1u << 8)
#define CFG_MODE_CONT   (0u << 8)
#define CFG_COMP_OFF    (0x3u)

#define I2C_TIMEOUT_MS  100

struct ads1115_t {
    i2c_master_dev_handle_t dev;
    uint16_t last_config;
};

/* ============================================================
 * Register access
 * ============================================================ */

static esp_err_t reg_write(ads1115_t *d, uint8_t reg, uint16_t value)
{
    /* The ADS1115 is big-endian on the wire. */
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(d->dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(ads1115_t *d, uint8_t reg, uint16_t *value)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(d->dev, &reg, 1, rx, 2,
                                                I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    *value = ((uint16_t)rx[0] << 8) | rx[1];
    return ESP_OK;
}

/* ============================================================
 * Lifecycle
 * ============================================================ */

esp_err_t ads1115_create(i2c_master_bus_handle_t bus, uint8_t addr,
                         ads1115_t **out)
{
    if (!bus || !out) return ESP_ERR_INVALID_ARG;

    ads1115_t *d = calloc(1, sizeof(*d));
    if (!d) return ESP_ERR_NO_MEM;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &d->dev);
    if (err != ESP_OK) {
        free(d);
        return err;
    }

    *out = d;
    return ESP_OK;
}

void ads1115_destroy(ads1115_t *d)
{
    if (!d) return;
    if (d->dev) i2c_master_bus_rm_device(d->dev);
    free(d);
}

esp_err_t ads1115_probe(ads1115_t *d)
{
    /* Write a known non-default pattern to the threshold-independent bits
     * of the config register and read it back. A device that merely ACKs
     * (a shorted bus, or the wrong chip at the same address) will not
     * reproduce the pattern. */
    const uint16_t pattern = 0x8583;   /* datasheet reset value with OS set */

    esp_err_t err = reg_write(d, REG_CONFIG, pattern);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no response at write: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t readback = 0;
    err = reg_read(d, REG_CONFIG, &readback);
    if (err != ESP_OK) return err;

    /* Bit 15 reads back as the conversion-ready flag rather than what was
     * written, so mask it out of the comparison. */
    if ((readback & 0x7FFF) != (pattern & 0x7FFF)) {
        ESP_LOGE(TAG, "config readback 0x%04X, expected 0x%04X",
                 readback & 0x7FFF, pattern & 0x7FFF);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "detected, config readback ok");
    return ESP_OK;
}

/* ============================================================
 * Conversions
 * ============================================================ */

static uint16_t build_config(uint8_t channel, ads_fsr_t fsr, ads_sps_t sps,
                             bool single_shot)
{
    uint16_t cfg = 0;
    cfg |= (uint16_t)((0x4u | (channel & 0x3u)) << 12);  /* single-ended MUX */
    cfg |= (uint16_t)((fsr & 0x7u) << 9);
    cfg |= single_shot ? CFG_MODE_SINGLE : CFG_MODE_CONT;
    cfg |= (uint16_t)((sps & 0x7u) << 5);
    cfg |= CFG_COMP_OFF;
    return cfg;
}

uint32_t ads1115_period_us(ads_sps_t sps)
{
    static const uint32_t us[8] = {
        125000, /*   8 SPS */
         62500, /*  16     */
         31250, /*  32     */
         15625, /*  64     */
          7813, /* 128     */
          4000, /* 250     */
          2105, /* 475     */
          1163, /* 860     */
    };
    return us[sps & 0x7];
}

esp_err_t ads1115_read_single(ads1115_t *d, uint8_t channel,
                              ads_fsr_t fsr, ads_sps_t sps, int16_t *raw)
{
    if (channel > 3 || !raw) return ESP_ERR_INVALID_ARG;

    uint16_t cfg = build_config(channel, fsr, sps, true) | CFG_OS_SINGLE;
    esp_err_t err = reg_write(d, REG_CONFIG, cfg);
    if (err != ESP_OK) return err;
    d->last_config = cfg;

    /* Wait the nominal conversion time plus 20%, then poll the OS bit.
     * Sleeping the whole time blind would waste most of a tick at high
     * data rates; polling alone would hammer the bus. */
    uint32_t wait_us = ads1115_period_us(sps) + ads1115_period_us(sps) / 5;
    if (wait_us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
        esp_rom_delay_us(wait_us % 1000);
    } else {
        esp_rom_delay_us(wait_us);
    }

    for (int attempt = 0; attempt < 10; attempt++) {
        uint16_t status;
        err = reg_read(d, REG_CONFIG, &status);
        if (err != ESP_OK) return err;
        if (status & CFG_OS_SINGLE) {        /* 1 = conversion complete */
            uint16_t v;
            err = reg_read(d, REG_CONVERSION, &v);
            if (err != ESP_OK) return err;
            *raw = (int16_t)v;
            return ESP_OK;
        }
        esp_rom_delay_us(200);
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t ads1115_start_continuous(ads1115_t *d, uint8_t channel,
                                   ads_fsr_t fsr, ads_sps_t sps)
{
    if (channel > 3) return ESP_ERR_INVALID_ARG;

    uint16_t cfg = build_config(channel, fsr, sps, false);
    esp_err_t err = reg_write(d, REG_CONFIG, cfg);
    if (err != ESP_OK) return err;
    d->last_config = cfg;

    /* The first conversion after a mode change is not valid until one full
     * period has elapsed; the caller would otherwise read a stale sample
     * from the previous channel and fold it into the RMS accumulator. */
    esp_rom_delay_us(ads1115_period_us(sps) * 2);
    return ESP_OK;
}

esp_err_t ads1115_read_conversion(ads1115_t *d, int16_t *raw)
{
    if (!raw) return ESP_ERR_INVALID_ARG;
    uint16_t v;
    esp_err_t err = reg_read(d, REG_CONVERSION, &v);
    if (err != ESP_OK) return err;
    *raw = (int16_t)v;
    return ESP_OK;
}

esp_err_t ads1115_stop_continuous(ads1115_t *d)
{
    /* Returning to single-shot leaves the device idle between conversions,
     * which drops supply current and stops it driving the bus. */
    return reg_write(d, REG_CONFIG, d->last_config | CFG_MODE_SINGLE);
}

float ads1115_to_volts(int16_t raw, ads_fsr_t fsr)
{
    /* PGA codes 5, 6 and 7 all select +/-0.256 V, so the table is 8 long
     * and the mask can never index past the end. */
    static const float full_scale[8] = {
        6.144f, 4.096f, 2.048f, 1.024f, 0.512f, 0.256f, 0.256f, 0.256f
    };
    return (float)raw * full_scale[fsr & 0x7] / 32768.0f;
}
