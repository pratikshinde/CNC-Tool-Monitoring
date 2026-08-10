/*
 * ads1115.h — TI ADS1115 16-bit delta-sigma ADC driver (I2C).
 *
 * Uses the ESP-IDF v5.2+ i2c_master API. The legacy driver/i2c.h interface
 * was removed in v6.0, so this will not build against the old one.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Programmable gain amplifier — full-scale range at the input pin. */
typedef enum {
    ADS_FSR_6144 = 0,   /* +/-6.144 V (exceeds VDD; not usable at 3V3) */
    ADS_FSR_4096 = 1,   /* +/-4.096 V (exceeds VDD; not usable at 3V3) */
    ADS_FSR_2048 = 2,   /* +/-2.048 V  <- the operating range for this board */
    ADS_FSR_1024 = 3,
    ADS_FSR_0512 = 4,
    ADS_FSR_0256 = 5,
} ads_fsr_t;

/* Data rate. The device has one ADC core shared by all four inputs, so
 * these are aggregate figures, not per-channel. */
typedef enum {
    ADS_SPS_8   = 0,
    ADS_SPS_16  = 1,
    ADS_SPS_32  = 2,
    ADS_SPS_64  = 3,
    ADS_SPS_128 = 4,
    ADS_SPS_250 = 5,
    ADS_SPS_475 = 6,
    ADS_SPS_860 = 7,
} ads_sps_t;

typedef struct ads1115_t ads1115_t;

/* Attach to an already-created I2C master bus. */
esp_err_t ads1115_create(i2c_master_bus_handle_t bus, uint8_t addr,
                         ads1115_t **out);

void ads1115_destroy(ads1115_t *dev);

/* Confirm the device answers and that its config register behaves.
 * A bare ACK is not enough — a shorted SDA will ACK everything. */
esp_err_t ads1115_probe(ads1115_t *dev);

/* One-shot conversion on a single-ended channel (0-3). Blocks for the
 * conversion time plus margin. */
esp_err_t ads1115_read_single(ads1115_t *dev, uint8_t channel,
                              ads_fsr_t fsr, ads_sps_t sps, int16_t *raw);

/* Put the device into continuous mode on one channel. Used for RMS bursts,
 * where the per-conversion command overhead of one-shot mode would eat
 * most of the available throughput. */
esp_err_t ads1115_start_continuous(ads1115_t *dev, uint8_t channel,
                                   ads_fsr_t fsr, ads_sps_t sps);

/* Read the conversion register. In continuous mode this returns the most
 * recent completed conversion; the caller is responsible for pacing reads
 * so it does not sample the same conversion twice. */
esp_err_t ads1115_read_conversion(ads1115_t *dev, int16_t *raw);

esp_err_t ads1115_stop_continuous(ads1115_t *dev);

/* Convert a raw code to volts for the given range. */
float ads1115_to_volts(int16_t raw, ads_fsr_t fsr);

/* Nominal conversion period in microseconds for a data rate. */
uint32_t ads1115_period_us(ads_sps_t sps);

#ifdef __cplusplus
}
#endif
