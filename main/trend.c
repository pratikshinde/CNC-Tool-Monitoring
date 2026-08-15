/*
 * trend.c
 */

#include "trend.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "monitor.h"

static const char *TAG = "trend";

typedef struct {
    trend_sample_t buf[TREND_CAPACITY];
    uint16_t       head;    /* next write position */
    uint16_t       count;
} trend_ring_t;

static trend_ring_t      s_ring[NUM_SPINDLES];
static SemaphoreHandle_t s_mutex;
static int64_t           s_last_sample_us;

static void trend_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        monitor_snapshot_t snap;
        monitor_get_snapshot(&snap);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (int i = 0; i < NUM_SPINDLES; i++) {
            trend_ring_t *r = &s_ring[i];
            r->buf[r->head] = (trend_sample_t){
                .current_a = snap.spindle[i].current_a,
                .pressure  = snap.spindle[i].pressure,
                .rpm       = snap.spindle[i].rpm,
            };
            r->head = (uint16_t)((r->head + 1) % TREND_CAPACITY);
            if (r->count < TREND_CAPACITY) r->count++;
        }
        s_last_sample_us = esp_timer_get_time();
        xSemaphoreGive(s_mutex);

        /* Absolute delay so the series stays on a steady 1 Hz grid even if
         * a sample is late — the graph's x-axis assumes even spacing. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TREND_PERIOD_MS));
    }
}

esp_err_t trend_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    memset(s_ring, 0, sizeof(s_ring));

    BaseType_t ok = xTaskCreatePinnedToCore(trend_task, "trend", 3072, NULL,
                                            PRIO_LOGGER, NULL, CORE_NETWORK);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "%d s of history at %d ms resolution",
             (TREND_CAPACITY * TREND_PERIOD_MS) / 1000, TREND_PERIOD_MS);
    return ESP_OK;
}

size_t trend_get(uint8_t spindle, trend_sample_t *out, size_t max,
                 uint32_t *age_ms_out)
{
    if (spindle >= NUM_SPINDLES || !out || max == 0) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    const trend_ring_t *r = &s_ring[spindle];
    size_t n = (r->count < max) ? r->count : max;

    /* Walk back n samples from the head, then emit forward so the caller
     * gets them oldest-first ready to plot left to right. */
    size_t start = (size_t)(r->head + TREND_CAPACITY - n) % TREND_CAPACITY;
    for (size_t i = 0; i < n; i++) {
        out[i] = r->buf[(start + i) % TREND_CAPACITY];
    }

    if (age_ms_out) {
        int64_t age_us = esp_timer_get_time() - s_last_sample_us;
        *age_ms_out = (s_last_sample_us == 0) ? 0 : (uint32_t)(age_us / 1000);
    }

    xSemaphoreGive(s_mutex);
    return n;
}
