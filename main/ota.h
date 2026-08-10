/*
 * ota.h — firmware update by upload from the web UI.
 *
 * Streaming, chunk at a time, because a firmware image is around a
 * megabyte and this device does not have a megabyte of RAM to buffer one.
 *
 * The rollback half of OTA already exists and is not repeated here:
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is set, and self_confirm_task() in
 * main.c only calls esp_ota_mark_app_valid_cancel_rollback() after the
 * monitor loop has proved it is actually running. So an image that flashes
 * successfully but then fails to measure is reverted by the bootloader on
 * the next boot without anyone having to intervene.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     in_progress;
    size_t   received;
    size_t   total;
    char     partition[16];
} ota_status_t;

/* Open the inactive OTA slot and prepare to receive `total_len` bytes.
 * `total_len` may be 0 if the client did not declare a length. */
esp_err_t ota_begin(size_t total_len);

/* Append one chunk. Any failure aborts the session internally, so the
 * caller only has to stop feeding it. */
esp_err_t ota_write(const void *data, size_t len);

/* Validate the image, set it as the boot partition and finish. The caller
 * is expected to reboot shortly afterwards. */
esp_err_t ota_end(void);

/* Abandon an update in progress and release the handle. */
void ota_abort(void);

void ota_get_status(ota_status_t *out);

#ifdef __cplusplus
}
#endif
