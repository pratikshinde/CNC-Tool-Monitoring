/*
 * ota.c
 */

#include "ota.h"

#include <inttypes.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota";

static esp_ota_handle_t       s_handle;
static const esp_partition_t *s_partition;
static bool                   s_in_progress;
static size_t                 s_received;
static size_t                 s_total;

/* Enough of the image to contain the magic byte and the app descriptor,
 * which is what we check before committing to a full erase. */
#define OTA_HEADER_CHECK_LEN \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + \
     sizeof(esp_app_desc_t))

static uint8_t s_header[OTA_HEADER_CHECK_LEN];
static size_t  s_header_len;
static bool    s_header_checked;

esp_err_t ota_begin(size_t total_len)
{
    if (s_in_progress) {
        ESP_LOGW(TAG, "update already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_partition) {
        ESP_LOGE(TAG, "no OTA partition available");
        return ESP_ERR_NOT_FOUND;
    }

    if (total_len > s_partition->size) {
        ESP_LOGE(TAG, "image of %u bytes will not fit partition %s (%" PRIu32 " bytes)",
                 (unsigned)total_len, s_partition->label, s_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* OTA_SIZE_UNKNOWN erases the whole partition, which is the honest
     * choice when the client did not send a Content-Length. */
    esp_err_t err = esp_ota_begin(s_partition,
                                  total_len ? total_len : OTA_SIZE_UNKNOWN,
                                  &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_in_progress    = true;
    s_received       = 0;
    s_total          = total_len;
    s_header_len     = 0;
    s_header_checked = false;

    ESP_LOGI(TAG, "receiving %u bytes into %s",
             (unsigned)total_len, s_partition->label);
    return ESP_OK;
}

/* Reject an image that is not for this project before it overwrites the
 * spare slot. esp_ota_end() would catch a corrupt image anyway, but only
 * after the entire upload has completed and the partition is already
 * ruined — better to fail on the first chunk. */
static esp_err_t check_header(void)
{
    const esp_image_header_t *img = (const esp_image_header_t *)s_header;

    if (img->magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "not a firmware image (magic 0x%02X)", img->magic);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *incoming = (const esp_app_desc_t *)
        (s_header + sizeof(esp_image_header_t) +
         sizeof(esp_image_segment_header_t));

    if (incoming->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "image has no application descriptor");
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (strncmp(incoming->project_name, running->project_name,
                sizeof(incoming->project_name)) != 0) {
        /* Flashing another project's image over a machine monitor would
         * brick it in a cabinet somewhere. Refuse. */
        ESP_LOGE(TAG, "image is for project '%.*s', this is '%.*s'",
                 (int)sizeof(incoming->project_name), incoming->project_name,
                 (int)sizeof(running->project_name), running->project_name);
        return ESP_ERR_INVALID_VERSION;
    }

    ESP_LOGI(TAG, "accepting %.*s version %.*s (running %.*s)",
             (int)sizeof(incoming->project_name), incoming->project_name,
             (int)sizeof(incoming->version), incoming->version,
             (int)sizeof(running->version), running->version);
    return ESP_OK;
}

esp_err_t ota_write(const void *data, size_t len)
{
    if (!s_in_progress) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_OK;

    /* Accumulate the header across however many chunks it takes; a client
     * is free to send it in pieces smaller than the descriptor. */
    if (!s_header_checked) {
        size_t want = OTA_HEADER_CHECK_LEN - s_header_len;
        size_t take = (len < want) ? len : want;
        memcpy(s_header + s_header_len, data, take);
        s_header_len += take;

        if (s_header_len == OTA_HEADER_CHECK_LEN) {
            esp_err_t err = check_header();
            if (err != ESP_OK) {
                ota_abort();
                return err;
            }
            s_header_checked = true;
        }
    }

    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed at %u bytes: %s",
                 (unsigned)s_received, esp_err_to_name(err));
        ota_abort();
        return err;
    }

    s_received += len;
    return ESP_OK;
}

esp_err_t ota_end(void)
{
    if (!s_in_progress) return ESP_ERR_INVALID_STATE;

    if (!s_header_checked) {
        ESP_LOGE(TAG, "upload ended before a complete image header arrived");
        ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err != ESP_OK) {
        /* ESP_ERR_OTA_VALIDATE_FAILED here means the image is corrupt —
         * a truncated upload or a bad connection. The running image is
         * untouched either way. */
        ESP_LOGE(TAG, "image validation failed: %s", esp_err_to_name(err));
        s_in_progress = false;
        return err;
    }

    err = esp_ota_set_boot_partition(s_partition);
    s_in_progress = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "update staged in %s (%u bytes) — reboot to apply",
             s_partition->label, (unsigned)s_received);
    return ESP_OK;
}

void ota_abort(void)
{
    if (!s_in_progress) return;
    if (s_handle) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    s_in_progress = false;
    ESP_LOGW(TAG, "update aborted after %u bytes", (unsigned)s_received);
}

void ota_get_status(ota_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->in_progress = s_in_progress;
    out->received    = s_received;
    out->total       = s_total;
    if (s_partition) {
        /* Deliberately truncating and always null-terminated; strncpy would
         * trip -Wstringop-truncation, which this build treats as an error. */
        size_t n = strnlen(s_partition->label, sizeof(out->partition) - 1);
        memcpy(out->partition, s_partition->label, n);
        out->partition[n] = '\0';
    }
}
