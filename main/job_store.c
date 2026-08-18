/*
 * job_store.c — see job_store.h.
 */
#include "job_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_littlefs.h"
#include "esp_log.h"

#include "alarm.h"   /* quantity_str() */

static const char *TAG = "job_store";

#define JOBS_MOUNT     "/jobs"
#define JOBS_PARTITION "jobs"
#define JOB_SUFFIX     ".json"

/* A template is ~3 KB. This cap exists so a corrupt or hostile file cannot
 * make the parser allocate without bound — not because a legitimate template
 * would ever approach it. */
#define JOB_FILE_MAX   16384

/* Written, then renamed over the real target — see job_store_save(). Leading
 * dot keeps it out of the listing, which only accepts `<name>.json`. */
#define JOB_TMP_PATH   JOBS_MOUNT "/.tmp"

static bool s_ready;

/* ============================================================
 * Name handling
 *
 * Template names arrive from an HTTP query parameter and become part of a
 * filesystem path, so they are untrusted input in the most direct sense: a
 * name of "../web/index.html" would escape the jobs partition entirely and
 * let a caller overwrite the served UI. Everything below treats the name as
 * hostile until it has passed job_name_is_safe().
 * ============================================================ */

static bool job_name_is_safe(const char *name)
{
    if (!name || name[0] == '\0') return false;

    size_t len = strlen(name);
    if (len >= JOB_NAME_MAX) return false;

    /* A leading dot would create a hidden file (and ".." is caught below);
     * neither is something an operator can have meant. */
    if (name[0] == '.') return false;

    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == ' ' || c == '_' || c == '-' || c == '.';
        if (!ok) return false;

        /* Belt and braces: the allowlist above already excludes '/' and '\\',
         * so no traversal sequence can be spelled — but ".." is rejected
         * explicitly so this stays correct if the allowlist is ever widened. */
        if (c == '.' && name[i + 1] == '.') return false;
    }
    return true;
}

static bool job_path_for(const char *name, char *out, size_t out_size)
{
    if (!job_name_is_safe(name)) return false;
    int n = snprintf(out, out_size, "%s/%s%s", JOBS_MOUNT, name, JOB_SUFFIX);
    return n > 0 && (size_t)n < out_size;
}

/* ============================================================
 * Small cJSON helpers
 *
 * Local copies of the same shapes web.c uses (its static set_float/set_u16/
 * add_band, web.c:218-241 and :655). Deliberately not shared: exposing web.c's
 * statics would invert the layering, and these are six-line functions. If
 * band_cfg_t ever gains a field, BOTH this file and web.c's add_band() need
 * updating — they serialize the same struct for different endpoints.
 * ============================================================ */

static void get_float(const cJSON *o, const char *key, float *dst)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(it)) *dst = (float)it->valuedouble;
}

static void get_u16(const cJSON *o, const char *key, uint16_t *dst)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(it)) *dst = (uint16_t)it->valueint;
}

static void get_u8(const cJSON *o, const char *key, uint8_t *dst)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(it)) *dst = (uint8_t)it->valueint;
}

static void get_bool(const cJSON *o, const char *key, bool *dst)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsBool(it)) *dst = cJSON_IsTrue(it);
}

static void get_str(const cJSON *o, const char *key, char *dst, size_t dst_size)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(it) && it->valuestring) {
        snprintf(dst, dst_size, "%s", it->valuestring);
    }
}

/* ============================================================
 * Serialization
 * ============================================================ */

static void band_to_json(cJSON *arr, const band_cfg_t *b)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", b->enabled);
    cJSON_AddNumberToObject(o, "limit", b->limit);
    cJSON_AddNumberToObject(o, "hysteresis", b->hysteresis);
    cJSON_AddNumberToObject(o, "on_delay_ms", b->on_delay_ms);
    cJSON_AddNumberToObject(o, "off_delay_ms", b->off_delay_ms);
    cJSON_AddBoolToObject(o, "latching", b->latching);
    cJSON_AddItemToArray(arr, o);
}

static void band_from_json(const cJSON *o, band_cfg_t *b)
{
    get_bool(o, "enabled", &b->enabled);
    get_float(o, "limit", &b->limit);
    get_float(o, "hysteresis", &b->hysteresis);
    get_u16(o, "on_delay_ms", &b->on_delay_ms);
    get_u16(o, "off_delay_ms", &b->off_delay_ms);
    get_bool(o, "latching", &b->latching);
}

static void wear_to_json(cJSON *dst, const wear_cfg_t *w)
{
    cJSON_AddBoolToObject(dst, "breakage_enabled", w->breakage_enabled);
    cJSON_AddNumberToObject(dst, "breakage_drop_pct", w->breakage_drop_pct);
    cJSON_AddNumberToObject(dst, "breakage_window_ms", w->breakage_window_ms);
    cJSON_AddBoolToObject(dst, "crash_enabled", w->crash_enabled);
    cJSON_AddNumberToObject(dst, "crash_rise_pct", w->crash_rise_pct);
    cJSON_AddNumberToObject(dst, "crash_window_ms", w->crash_window_ms);
    cJSON_AddBoolToObject(dst, "trend_enabled", w->trend_enabled);
    cJSON_AddNumberToObject(dst, "trend_cycles", w->trend_cycles);
    cJSON_AddNumberToObject(dst, "baseline_learn_cycles", w->baseline_learn_cycles);
    cJSON_AddNumberToObject(dst, "baseline_sigma_floor_pct", w->baseline_sigma_floor_pct);
    cJSON_AddNumberToObject(dst, "baseline_sigma_ceiling_pct", w->baseline_sigma_ceiling_pct);
    cJSON_AddNumberToObject(dst, "adaptive_k_warn", w->adaptive_k_warn);
    cJSON_AddNumberToObject(dst, "adaptive_k_alarm", w->adaptive_k_alarm);
}

static void wear_from_json(const cJSON *o, wear_cfg_t *w)
{
    get_bool(o, "breakage_enabled", &w->breakage_enabled);
    get_u8(o, "breakage_drop_pct", &w->breakage_drop_pct);
    get_u16(o, "breakage_window_ms", &w->breakage_window_ms);
    get_bool(o, "crash_enabled", &w->crash_enabled);
    get_u8(o, "crash_rise_pct", &w->crash_rise_pct);
    get_u16(o, "crash_window_ms", &w->crash_window_ms);
    get_bool(o, "trend_enabled", &w->trend_enabled);
    get_u8(o, "trend_cycles", &w->trend_cycles);
    get_u8(o, "baseline_learn_cycles", &w->baseline_learn_cycles);
    get_u8(o, "baseline_sigma_floor_pct", &w->baseline_sigma_floor_pct);
    get_u8(o, "baseline_sigma_ceiling_pct", &w->baseline_sigma_ceiling_pct);
    get_float(o, "adaptive_k_warn", &w->adaptive_k_warn);
    get_float(o, "adaptive_k_alarm", &w->adaptive_k_alarm);
}

cJSON *job_record_to_json(const job_record_t *rec)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    /* A fixed format tag so an import can tell a job template apart from any
     * other JSON a technician might pick in the file dialog, and refuse it
     * with a clear message rather than a parse-shaped one. */
    cJSON_AddStringToObject(root, "format", "cnc-tool-monitor-job");
    cJSON_AddNumberToObject(root, "schema_version", rec->schema_version);
    cJSON_AddStringToObject(root, "name", rec->meta.name);
    cJSON_AddStringToObject(root, "notes", rec->meta.notes);
    cJSON_AddStringToObject(root, "saved_at", rec->meta.saved_at);
    cJSON_AddStringToObject(root, "saved_by", rec->meta.saved_by);

    cJSON *spindles = cJSON_AddArrayToObject(root, "spindles");
    for (int i = 0; i < NUM_SPINDLES; i++) {
        if (!rec->present[i]) continue;

        const job_profile_t *p = &rec->profile[i];
        const job_context_t *c = &rec->context[i];

        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "index", i);

        /* Recorded so an import can be CHECKED against the destination
         * machine — never applied. See job_template.h. */
        cJSON *ctx = cJSON_AddObjectToObject(s, "context");
        cJSON_AddStringToObject(ctx, "pressure_unit", c->pressure_unit);
        cJSON_AddNumberToObject(ctx, "pressure_sensor_min", c->pressure_sensor_min);
        cJSON_AddNumberToObject(ctx, "pressure_sensor_max", c->pressure_sensor_max);
        cJSON_AddNumberToObject(ctx, "ct_primary_amps", c->ct_primary_amps);

        cJSON *job = cJSON_AddObjectToObject(s, "job");
        cJSON_AddBoolToObject(job, "enabled", p->enabled);
        cJSON_AddBoolToObject(job, "machine_running_enabled", p->machine_running_enabled);
        cJSON_AddNumberToObject(job, "min_pulse_ms", p->min_pulse_ms);

        cJSON *sm = cJSON_AddObjectToObject(job, "sm");
        cJSON_AddNumberToObject(sm, "start_rpm", p->sm.start_rpm);
        cJSON_AddNumberToObject(sm, "settle_ms", p->sm.settle_ms);
        cJSON_AddNumberToObject(sm, "idle_current_a", p->sm.idle_current_a);
        cJSON_AddNumberToObject(sm, "cut_detect_current_a", p->sm.cut_detect_current_a);
        cJSON_AddNumberToObject(sm, "alarm_inhibit_ms", p->sm.alarm_inhibit_ms);

        wear_to_json(cJSON_AddObjectToObject(job, "wear"), &p->wear);

        cJSON *bands = cJSON_AddObjectToObject(job, "bands");
        for (int q = 0; q < QTY_COUNT; q++) {
            cJSON *arr = cJSON_AddArrayToObject(bands, quantity_str((quantity_t)q));
            for (int b = 0; b < BAND_COUNT; b++) {
                band_to_json(arr, &p->bands[q][b]);
            }
        }

        cJSON_AddItemToArray(spindles, s);
    }

    return root;
}

esp_err_t job_record_from_json(const cJSON *root, job_record_t *out)
{
    if (!cJSON_IsObject(root)) return ESP_ERR_INVALID_RESPONSE;

    const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
    if (!cJSON_IsString(fmt) || !fmt->valuestring ||
        strcmp(fmt->valuestring, "cnc-tool-monitor-job") != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));

    /* Every profile starts from factory defaults, so an OLDER template that
     * simply lacks a field applies a sane value for it rather than a zero.
     * job_check() separately refuses anything NEWER than this firmware. */
    for (int i = 0; i < NUM_SPINDLES; i++) {
        spindle_cfg_t defaults;
        spindle_cfg_set_defaults(&defaults, i);
        job_profile_extract(&defaults, &out->profile[i]);
    }

    get_u16(root, "schema_version", &out->schema_version);
    get_str(root, "name", out->meta.name, sizeof(out->meta.name));
    get_str(root, "notes", out->meta.notes, sizeof(out->meta.notes));
    get_str(root, "saved_at", out->meta.saved_at, sizeof(out->meta.saved_at));
    get_str(root, "saved_by", out->meta.saved_by, sizeof(out->meta.saved_by));

    const cJSON *spindles = cJSON_GetObjectItemCaseSensitive(root, "spindles");
    if (!cJSON_IsArray(spindles)) return ESP_ERR_INVALID_RESPONSE;

    const cJSON *s = NULL;
    cJSON_ArrayForEach(s, spindles) {
        if (!cJSON_IsObject(s)) continue;

        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(s, "index");
        if (!cJSON_IsNumber(idx)) continue;
        int i = idx->valueint;
        if (i < 0 || i >= NUM_SPINDLES) continue;   /* a template from a
                                                      * wider machine: take
                                                      * the spindles we have */

        job_profile_t *p = &out->profile[i];
        job_context_t *c = &out->context[i];

        const cJSON *ctx = cJSON_GetObjectItemCaseSensitive(s, "context");
        if (cJSON_IsObject(ctx)) {
            get_str(ctx, "pressure_unit", c->pressure_unit, sizeof(c->pressure_unit));
            get_float(ctx, "pressure_sensor_min", &c->pressure_sensor_min);
            get_float(ctx, "pressure_sensor_max", &c->pressure_sensor_max);
            get_float(ctx, "ct_primary_amps", &c->ct_primary_amps);
        }

        const cJSON *job = cJSON_GetObjectItemCaseSensitive(s, "job");
        if (!cJSON_IsObject(job)) continue;

        get_bool(job, "enabled", &p->enabled);
        get_bool(job, "machine_running_enabled", &p->machine_running_enabled);
        get_u16(job, "min_pulse_ms", &p->min_pulse_ms);

        const cJSON *sm = cJSON_GetObjectItemCaseSensitive(job, "sm");
        if (cJSON_IsObject(sm)) {
            get_float(sm, "start_rpm", &p->sm.start_rpm);
            get_u16(sm, "settle_ms", &p->sm.settle_ms);
            get_float(sm, "idle_current_a", &p->sm.idle_current_a);
            get_float(sm, "cut_detect_current_a", &p->sm.cut_detect_current_a);
            get_u16(sm, "alarm_inhibit_ms", &p->sm.alarm_inhibit_ms);
        }

        const cJSON *wear = cJSON_GetObjectItemCaseSensitive(job, "wear");
        if (cJSON_IsObject(wear)) wear_from_json(wear, &p->wear);

        const cJSON *bands = cJSON_GetObjectItemCaseSensitive(job, "bands");
        if (cJSON_IsObject(bands)) {
            for (int q = 0; q < QTY_COUNT; q++) {
                const cJSON *arr = cJSON_GetObjectItemCaseSensitive(
                    bands, quantity_str((quantity_t)q));
                if (!cJSON_IsArray(arr)) continue;
                for (int b = 0; b < BAND_COUNT; b++) {
                    const cJSON *o = cJSON_GetArrayItem(arr, b);
                    if (cJSON_IsObject(o)) band_from_json(o, &p->bands[q][b]);
                }
            }
        }

        out->present[i] = true;
    }

    /* A template that captured no spindle at all is not usable for anything. */
    bool any = false;
    for (int i = 0; i < NUM_SPINDLES; i++) any = any || out->present[i];
    return any ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

void job_record_capture(const app_config_t *cfg, const job_meta_t *meta,
                        job_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->meta = *meta;
    out->schema_version = CONFIG_SCHEMA_VERSION;

    for (int i = 0; i < NUM_SPINDLES; i++) {
        job_profile_extract(&cfg->spindle[i], &out->profile[i]);
        job_context_extract(&cfg->spindle[i], &out->context[i]);
        out->present[i] = true;
    }
}

/* ============================================================
 * Filesystem
 * ============================================================ */

esp_err_t job_store_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = JOBS_MOUNT,
        .partition_label = JOBS_PARTITION,
        /* TRUE here, unlike web.c's mount of the UI partition: a blank jobs
         * filesystem is the normal state of a factory-fresh unit, not the
         * corrupt-image symptom it would be for the web partition (which is
         * always written by the flash step). Refusing to format would leave
         * every new unit unable to save a template until someone flashed an
         * empty image at it. */
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to mount jobs partition: %s", esp_err_to_name(err));
        s_ready = false;
        return err;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(JOBS_PARTITION, &total, &used);
    ESP_LOGI(TAG, "jobs partition mounted: %u/%u bytes used",
             (unsigned)used, (unsigned)total);

    s_ready = true;
    return ESP_OK;
}

bool job_store_ready(void)
{
    return s_ready;
}

/* Reads a whole file into a NUL-terminated heap buffer. Caller frees. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0 || len > JOB_FILE_MAX) { fclose(f); return NULL; }
    rewind(f);

    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

cJSON *job_store_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    if (!s_ready) return arr;

    DIR *d = opendir(JOBS_MOUNT);
    if (!d) return arr;

    /* Each file is opened and parsed in turn purely to read its four
     * metadata strings, then freed before the next — so peak heap is one
     * template, not the whole partition. It is O(n) file reads for a listing,
     * which is fine for an operator-initiated action on a set that is
     * realistically tens of templates, and avoids a separate index file that
     * could drift out of sync with the directory. */
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *fn = e->d_name;
        size_t len = strlen(fn);
        size_t suffix_len = strlen(JOB_SUFFIX);
        if (len <= suffix_len) continue;
        if (strcmp(fn + len - suffix_len, JOB_SUFFIX) != 0) continue;

        char path[64 + JOB_NAME_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", JOBS_MOUNT, fn) <= 0) continue;

        char *text = read_file(path);
        if (!text) continue;

        cJSON *root = cJSON_Parse(text);
        free(text);
        if (!root) continue;

        job_record_t rec;
        if (job_record_from_json(root, &rec) == ESP_OK) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "name", rec.meta.name);
            cJSON_AddStringToObject(m, "notes", rec.meta.notes);
            cJSON_AddStringToObject(m, "saved_at", rec.meta.saved_at);
            cJSON_AddStringToObject(m, "saved_by", rec.meta.saved_by);
            cJSON_AddNumberToObject(m, "schema_version", rec.schema_version);
            cJSON *sp = cJSON_AddArrayToObject(m, "spindles");
            for (int i = 0; i < NUM_SPINDLES; i++) {
                if (rec.present[i]) cJSON_AddItemToArray(sp, cJSON_CreateNumber(i));
            }
            cJSON_AddItemToArray(arr, m);
        }
        cJSON_Delete(root);
    }

    closedir(d);
    return arr;
}

esp_err_t job_store_load(const char *name, job_record_t *out)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char path[64 + JOB_NAME_MAX];
    if (!job_path_for(name, path, sizeof(path))) return ESP_ERR_INVALID_ARG;

    char *text = read_file(path);
    if (!text) return ESP_ERR_NOT_FOUND;

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    esp_err_t err = job_record_from_json(root, out);
    cJSON_Delete(root);
    return err;
}

esp_err_t job_store_save(const job_record_t *rec)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char path[64 + JOB_NAME_MAX];
    if (!job_path_for(rec->meta.name, path, sizeof(path))) return ESP_ERR_INVALID_ARG;

    cJSON *root = job_record_to_json(rec);
    if (!root) return ESP_ERR_NO_MEM;

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;

    /* Write to a temporary file and rename over the target. A power loss
     * mid-write then leaves the PREVIOUS version of the template intact
     * rather than a half-written one — the same reasoning behind
     * config_store.c's two NVS slots and smu_store.c's alternating flash
     * pages, using the rename a filesystem gives us for free. */
    FILE *f = fopen(JOB_TMP_PATH, "wb");
    if (!f) { free(text); return ESP_FAIL; }

    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    free(text);

    /* fclose() is where buffered data actually reaches the media, so its
     * result matters as much as fwrite's — a full filesystem typically
     * surfaces here, not earlier. */
    bool ok = (written == len) && (fclose(f) == 0);
    if (!ok) {
        remove(JOB_TMP_PATH);
        ESP_LOGE(TAG, "write failed for '%s' (partition full?)", rec->meta.name);
        return ESP_FAIL;
    }

    /* LittleFS rename replaces an existing destination atomically. */
    remove(path);
    if (rename(JOB_TMP_PATH, path) != 0) {
        remove(JOB_TMP_PATH);
        ESP_LOGE(TAG, "rename failed for '%s'", rec->meta.name);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved job template '%s' (%u bytes)",
             rec->meta.name, (unsigned)len);
    return ESP_OK;
}

esp_err_t job_store_delete(const char *name)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    char path[64 + JOB_NAME_MAX];
    if (!job_path_for(name, path, sizeof(path))) return ESP_ERR_INVALID_ARG;

    if (remove(path) != 0) return ESP_ERR_NOT_FOUND;

    ESP_LOGI(TAG, "deleted job template '%s'", name);
    return ESP_OK;
}
