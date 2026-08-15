/*
 * main.c — CNC Tool Monitor entry point.
 *
 * Bring-up order matters and is not arbitrary:
 *
 *   1. config    — everything else needs it, and a bad config must be
 *                  caught before it can drive an output
 *   2. monitor   — starts the telemetry task. V1 had dio/analog/rpm
 *                  bring-up steps here too; V2 retires all three from the
 *                  ESP32 (FIRMWARE_DESIGN_SPEC.md §2.3) — acquisition,
 *                  arming and output-driving all move to the SMU. This
 *                  step is presently the Phase 2 placeholder monitor.c
 *                  describes in its own header comment, pending Phase
 *                  3's real SMU link.
 *   3. wifi, modbus, web — networking, all on CORE_NETWORK so nothing here
 *                  can delay the real-time loop on core 0
 *
 * The data logger is not started here yet; it belongs to the next phase.
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_config.h"
#include "board.h"
#include "config_store.h"
#include "modbus.h"
#include "monitor.h"
#include "trend.h"
#include "web.h"
#include "wifi.h"

static const char *TAG = "main";

/* How long the application must run cleanly before it declares itself
 * healthy: cancels the OTA rollback and promotes the active config to
 * known-good. Long enough to have completed several measurement loops and
 * shaken out an immediate crash, short enough that a genuine update is
 * confirmed while the engineer is still standing at the machine. */
#define SELF_CONFIRM_DELAY_MS  30000

static void log_reset_reason(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    const char *s;

    switch (r) {
    case ESP_RST_POWERON:  s = "power-on";           break;
    case ESP_RST_SW:       s = "software restart";   break;
    case ESP_RST_PANIC:    s = "panic";              break;
    case ESP_RST_INT_WDT:  s = "interrupt watchdog"; break;
    case ESP_RST_TASK_WDT: s = "task watchdog";      break;
    case ESP_RST_WDT:      s = "other watchdog";     break;
    case ESP_RST_BROWNOUT: s = "brown-out";          break;
    default:               s = "other";              break;
    }

    /* A watchdog or panic reset is evidence of a firmware defect and must
     * not scroll past unnoticed — it goes in the event journal too, once
     * that exists (DG-R5). */
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
        r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
        ESP_LOGE(TAG, "*** previous run ended abnormally: %s ***", s);
    } else {
        ESP_LOGI(TAG, "reset reason: %s", s);
    }
}

static void self_confirm_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(SELF_CONFIRM_DELAY_MS));

    monitor_snapshot_t snap;
    monitor_get_snapshot(&snap);

    /* Only confirm if the real-time loop is actually running. A build that
     * boots but never measures should be rolled back, not blessed. */
    if (snap.loop_period_ms == 0) {
        ESP_LOGE(TAG, "monitor loop not running — withholding OTA confirmation");
        vTaskDelete(NULL);
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "firmware confirmed healthy, rollback cancelled");
    }

    if (config_store_mark_good() == ESP_OK) {
        ESP_LOGI(TAG, "configuration promoted to known-good");
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " CNC Tool Monitor  %s  (%s %s)",
             app->version, app->date, app->time);
    ESP_LOGI(TAG, " %d spindles | IDF %s", NUM_SPINDLES, app->idf_ver);
    ESP_LOGI(TAG, "==================================================");

    log_reset_reason();

    /* --- 1. Configuration ------------------------------------------- */

    config_load_source_t src;
    ESP_ERROR_CHECK(config_store_init(&src));

    switch (src) {
    case CFG_LOADED_ACTIVE:
        ESP_LOGI(TAG, "configuration loaded");
        break;
    case CFG_LOADED_KNOWN_GOOD:
        ESP_LOGW(TAG, "configuration recovered from known-good copy");
        break;
    case CFG_LOADED_DEFAULTS:
        ESP_LOGW(TAG, "running on FACTORY DEFAULTS — commissioning required");
        break;
    }

    const app_config_t *cfg = config_get();

    /* --- 2. Telemetry task ---------------------------------------------
     * V1's dio_init() / analog_init() / rpm_init() bring-up steps are
     * gone — see the file header comment and monitor.c. */

    ESP_ERROR_CHECK(monitor_start());

    /* --- 3. Networking -------------------------------------------------
     * All on CORE_NETWORK. A WiFi or HTTP fault here must not be able to
     * touch the real-time loop or the outputs it already released above. */

    ESP_ERROR_CHECK(trend_start());
    ESP_ERROR_CHECK(wifi_init(cfg));
    ESP_ERROR_CHECK(modbus_init(cfg));
    ESP_ERROR_CHECK(web_init(cfg));

    xTaskCreatePinnedToCore(self_confirm_task, "confirm", 3072, NULL,
                            2, NULL, CORE_NETWORK);

    ESP_LOGI(TAG, "running");

    /* Console heartbeat until the web UI exists. Deliberately on core 1 so
     * the formatting cost never lands on the real-time loop. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        monitor_snapshot_t s;
        monitor_get_snapshot(&s);

        for (int i = 0; i < NUM_SPINDLES; i++) {
            if (!cfg->spindle[i].enabled) continue;
            const spindle_snapshot_t *sp = &s.spindle[i];

            printf("S%d %-10s %6.2f A (avg %6.2f, pk %6.2f)  "
                   "%7.1f %-4s  %8.0f rpm  %s%s\n",
                   i, spindle_state_str(sp->state),
                   sp->current_a, sp->current_avg_a, sp->current_peak_a,
                   sp->pressure, cfg->spindle[i].pressure.unit,
                   sp->rpm,
                   sp->severity == SEV_NONE ? "-" : severity_str(sp->severity),
                   sp->monitoring_armed ? " [armed]" : "");
        }

        printf("   loop %lu ms (worst %lu)  adc:%s  health:%s\n\n",
               (unsigned long)s.loop_period_ms,
               (unsigned long)s.worst_loop_ms,
               s.adc_healthy ? "ok" : "FAIL",
               s.system_healthy ? "ok" : "FAULT");
    }
}
