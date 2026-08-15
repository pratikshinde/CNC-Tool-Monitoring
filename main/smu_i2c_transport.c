/*
 * smu_i2c_transport.c — see smu_i2c_transport.h.
 *
 * Wire format per smu_proto.h: the register pointer is 16-bit,
 * big-endian, sent as its own two bytes before the payload. A read is
 * therefore "transmit the 2-byte pointer, then receive `len` bytes" in
 * one bus transaction (i2c_master_transmit_receive, which issues a
 * repeated START rather than a STOP between the two halves — required
 * here, since a STOP would let another bus master interleave a
 * transaction on this SMU's address between the pointer write and the
 * read, on a bus this driver does not otherwise share with anything).
 * A write is "transmit [reg_hi, reg_lo, ...payload]" as a single buffer.
 */

#include "smu_i2c_transport.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"

/* Generous relative to a 100 kHz bus and the largest single transaction
 * (the 296-byte config block, ~27 ms of raw bit time) — this bounds a
 * stuck/NAKing device, not normal operation. */
#define SMU_I2C_XFER_TIMEOUT_MS   100

/* Largest single write payload is the config block (296 B, see
 * smu_proto.h's SMU_REG_CONFIG_WINDOW) plus the 2-byte register pointer
 * prefixed onto it below. */
#define SMU_I2C_WRITE_BUF_MAX     (2 + 296)

static esp_err_t i2c_read(void *ctx, uint16_t reg, void *out, size_t len)
{
    smu_i2c_ctx_t *c = (smu_i2c_ctx_t *)ctx;
    uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };

    return i2c_master_transmit_receive(c->dev, ptr, sizeof(ptr),
                                       (uint8_t *)out, len,
                                       SMU_I2C_XFER_TIMEOUT_MS);
}

static esp_err_t i2c_write(void *ctx, uint16_t reg, const void *data, size_t len)
{
    smu_i2c_ctx_t *c = (smu_i2c_ctx_t *)ctx;
    if (len > SMU_I2C_WRITE_BUF_MAX - 2) return ESP_ERR_INVALID_SIZE;

    uint8_t buf[SMU_I2C_WRITE_BUF_MAX];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    memcpy(&buf[2], data, len);

    return i2c_master_transmit(c->dev, buf, len + 2, SMU_I2C_XFER_TIMEOUT_MS);
}

static void i2c_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    /* vTaskDelay's granularity is one tick (1 ms at this project's
     * configured CONFIG_FREERTOS_HZ=1000 — see sdkconfig.defaults), which
     * is coarser than a raw microsecond delay would be. That is fine
     * here: this only paces smu_link_push_config()'s commit-completion
     * poll, not the 20 Hz telemetry path, so sub-millisecond precision
     * buys nothing. Round up so a request for a few hundred us still
     * yields at least one real tick instead of rounding to zero. */
    uint32_t ms = (us + 999) / 1000;
    if (ms == 0) ms = 1;
    vTaskDelay(pdMS_TO_TICKS(ms));
}

esp_err_t smu_i2c_transport_init(smu_i2c_ctx_t *ctx, int port,
                                 gpio_num_t sda, gpio_num_t scl,
                                 uint16_t device_address,
                                 smu_transport_t *out)
{
    memset(ctx, 0, sizeof(*ctx));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = port,
        .sda_io_num        = sda,
        .scl_io_num        = scl,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &ctx->bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = device_address,
        .scl_speed_hz    = SMU_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(ctx->bus, &dev_cfg, &ctx->dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(ctx->bus);
        ctx->bus = NULL;
        return err;
    }

    out->read     = i2c_read;
    out->write    = i2c_write;
    out->delay_us = i2c_delay;
    out->ctx      = ctx;
    return ESP_OK;
}
