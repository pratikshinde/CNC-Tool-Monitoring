/*
 * smu_i2c_transport.h — real I2C implementation of smu_transport_t.
 *
 * The counterpart to smu_mock.c: same smu_transport_t seam, backed by
 * actual silicon instead of a synthesized spindle. smu_link.c's protocol
 * logic (CRC, staleness, the two-phase commit) does not know or care
 * which one it is talking to.
 *
 * Uses the new i2c_master driver (driver/i2c_master.h) — this ESP-IDF
 * install has no legacy driver/i2c.h at all, only i2c_master.h/
 * i2c_slave.h, so there was never a choice to make here.
 *
 * Unverified against real SMU hardware as of the V2 migration Phase 3
 * that wrote this file — no SMU exists yet to test against. This is the
 * accepted risk the migration plan named explicitly: protocol logic is
 * host-tested (smu_link.c), the transport itself is not, and will not be
 * until real hardware exists. Reviewed carefully anyway; not exercised
 * for real.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "smu_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} smu_i2c_ctx_t;

/* Creates a new I2C master bus on `port` and adds the SMU as a device at
 * `device_address` (7-bit). `ctx` must outlive the returned transport —
 * it holds the driver handles the transport's read/write calls use.
 * Each SMU gets its own bus (board.h's SMU1_I2C_* / SMU2_I2C_* — not a
 * shared multi-drop bus), so this is called once per SMU, not once
 * overall. */
esp_err_t smu_i2c_transport_init(smu_i2c_ctx_t *ctx, int port,
                                 gpio_num_t sda, gpio_num_t scl,
                                 uint16_t device_address,
                                 smu_transport_t *out);

#ifdef __cplusplus
}
#endif
