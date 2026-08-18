/*
 * job_store.h — saved job templates on the `jobs` LittleFS partition.
 *
 * Storage and serialization only. The rules about WHAT may travel between
 * machines — and the compatibility checking — live in job_template.[ch],
 * deliberately separate so that logic stays free of cJSON and the filesystem
 * and can be exercised under host_test/.
 *
 * On-disk layout: one JSON file per template, `/jobs/<name>.json`. JSON rather
 * than a packed binary because the web UI already speaks it (so save/load
 * reuses the existing cJSON paths), because a technician can read and email
 * an exported file, and because the space saving from packing would be a few
 * hundred bytes against a 512 KB partition.
 *
 * Why its own partition rather than `web`: see partitions.csv. A UI update
 * replaces the `web` partition wholesale and would take every saved template
 * with it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_err.h"
#include "job_template.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest template name an operator may type. Bounded well under LittleFS's
 * own 255-byte filename limit so `<name>.json` plus the mount prefix always
 * fits, with room for the sanitiser to escape characters if needed. */
#define JOB_NAME_MAX   48
#define JOB_NOTES_MAX  96
/* Free-text supplied by the browser at save time — the device has no RTC
 * (the DS3231 was retired in V2), so it cannot timestamp anything itself. */
#define JOB_STAMP_MAX  32

typedef struct {
    char name[JOB_NAME_MAX];
    char notes[JOB_NOTES_MAX];
    char saved_at[JOB_STAMP_MAX];
    char saved_by[JOB_NAME_MAX];
} job_meta_t;

/* One template: metadata plus a profile and authored context per spindle. */
typedef struct {
    job_meta_t    meta;
    uint16_t      schema_version;
    job_profile_t profile[NUM_SPINDLES];
    job_context_t context[NUM_SPINDLES];
    bool          present[NUM_SPINDLES];   /* false = this spindle was not captured */
} job_record_t;

/* Mounts the `jobs` partition read-write at /jobs. Call once at boot, before
 * web_init(). Unlike the web partition's mount, format_if_mount_failed is
 * TRUE here: an empty jobs filesystem on a factory-fresh unit is the normal
 * first-boot state, not a corrupt-image symptom. */
esp_err_t job_store_init(void);

/* True once job_store_init() has succeeded — the HTTP handlers use this to
 * return a clear "job storage unavailable" rather than failing obscurely on
 * every file operation. */
bool job_store_ready(void);

/* Enumerate saved templates into a freshly created cJSON array of metadata
 * objects. Caller owns the returned array. Returns NULL on allocation
 * failure. Reads each file's header fields only — a listing does not parse
 * or validate the profiles. */
cJSON *job_store_list(void);

/* Load one template by name. Returns ESP_ERR_NOT_FOUND if it does not exist,
 * ESP_ERR_INVALID_RESPONSE if the file is unparseable or not a job template. */
esp_err_t job_store_load(const char *name, job_record_t *out);

/* Serialize and write. Overwrites an existing template of the same name.
 * Writes to a temporary file and renames over the target, so a power loss
 * mid-write leaves the previous version intact rather than a truncated one —
 * the same reasoning as config_store.c's two-slot scheme and smu_store.c's
 * alternating pages, applied to a filesystem that gives us rename for free. */
esp_err_t job_store_save(const job_record_t *rec);

esp_err_t job_store_delete(const char *name);

/* Build the wire/file JSON for a record (used for both storage and the
 * export endpoint), and parse it back (used for both load and import).
 * Exposed so the HTTP layer can hand an imported body straight to the parser
 * without a round-trip through the filesystem. */
cJSON    *job_record_to_json(const job_record_t *rec);
esp_err_t job_record_from_json(const cJSON *root, job_record_t *out);

/* Capture the current live config of every spindle into a record. */
void job_record_capture(const app_config_t *cfg, const job_meta_t *meta,
                        job_record_t *out);

#ifdef __cplusplus
}
#endif
