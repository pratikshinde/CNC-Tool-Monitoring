/*
 * modbus.c
 *
 * Register map (holding registers, function codes 03/04, read-only):
 *
 *   0        schema version of the config that produced these readings
 *              (CONFIG_SCHEMA_VERSION) — lets an integrator sanity-check
 *              the register layout matches what they expect.
 *   1        system_healthy      (0/1)
 *   2        adc_healthy         (0/1)
 *   3        diagnostic_fault    (0/1)
 *   4        loop_period_ms (clamped to 65535)
 *
 *   100 + spindle*50   per-spindle block (spindle 0 -> 100, spindle 1 -> 150)
 *     +0   enabled              (0/1)
 *     +1   state                (spindle_state_t, spindle_sm.h)
 *     +2   monitoring_armed     (0/1)
 *     +3   current_a      x100
 *     +4   current_avg_a  x100
 *     +5   current_peak_a x100
 *     +6   pressure       x100
 *     +7   rpm                  (clamped to 65535)
 *     +8   severity             (severity_t, alarm.h)
 *     +9   any_alarm            (0/1)
 *     +10  any_warning          (0/1)
 *     +11  current_status       (sensor_status_t, analog.h)
 *     +12  pressure_status      (sensor_status_t)
 *     +13  rpm_sensor_suspect   (0/1)
 *     +14  cycle_count hi 16 bits
 *     +15  cycle_count lo 16 bits
 *     +16  last_cycle_ms (clamped to 65535)
 *     +17  last_cycle_mean_a x100
 *
 * 50-register spacing per spindle leaves headroom without redesigning the
 * map later, and lines up with PLC-friendly 40101/40151-style addressing.
 *
 * RTU and TCP are two independent esp-modbus slave instances that both
 * point their holding-register descriptor at the same backing array
 * (s_holding_regs) — esp-modbus v2's per-instance API (mbc_slave_create_serial
 * / mbc_slave_create_tcp, each returning its own opaque handle) supports
 * this directly, unlike the older single-instance mbcontroller API. This
 * is a monitoring device, so registers are MB_ACCESS_RO: nothing on the
 * bus can write into the alarm/telemetry state.
 */

#include "modbus.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "mbcontroller.h"

#include "board.h"
#include "config_store.h"
#include "monitor.h"

static const char *TAG = "modbus";

#define MB_REG_SYS_BASE       0
#define MB_REG_SPINDLE_BASE   100
#define MB_REG_SPINDLE_STRIDE 50
#define MB_HOLDING_REG_COUNT  (MB_REG_SPINDLE_BASE + NUM_SPINDLES * MB_REG_SPINDLE_STRIDE)

#define MB_UPDATE_PERIOD_MS   200

static void        *s_rtu_handle;
static void         *s_tcp_handle;
static TaskHandle_t  s_task;
static uint16_t      s_holding_regs[MB_HOLDING_REG_COUNT];

/* Guards s_rtu_handle/s_tcp_handle themselves (not the register contents —
 * that's mbc_slave_lock/unlock, below). Without this, a settings save
 * (modbus_reconfigure, called from the HTTP task) can delete and recreate
 * the handles while modbus_task is mid-way through reading them on its
 * 200 ms cycle, which is a use-after-free: both tasks run on CORE_NETWORK,
 * so a context switch between the NULL-check and the mbc_slave_lock() call
 * is entirely possible even without a second core involved. */
static SemaphoreHandle_t s_handle_mutex;

/* ============================================================
 * Register refresh
 * ============================================================ */

static uint16_t scale100(float v)
{
    float scaled = v * 100.0f + 0.5f;
    if (scaled < 0.0f)     return 0;
    if (scaled > 65535.0f) return 65535;
    return (uint16_t)scaled;
}

static uint16_t clamp_u16(uint32_t v)
{
    return (v > 65535u) ? 65535u : (uint16_t)v;
}

static void update_registers(const monitor_snapshot_t *snap, const app_config_t *cfg)
{
    uint16_t regs[MB_HOLDING_REG_COUNT];
    memset(regs, 0, sizeof(regs));

    regs[0] = CONFIG_SCHEMA_VERSION;
    regs[1] = snap->system_healthy   ? 1 : 0;
    regs[2] = snap->adc_healthy      ? 1 : 0;
    regs[3] = snap->diagnostic_fault ? 1 : 0;
    regs[4] = clamp_u16(snap->loop_period_ms);

    for (int i = 0; i < NUM_SPINDLES; i++) {
        const spindle_snapshot_t *sp = &snap->spindle[i];
        uint16_t *r = &regs[MB_REG_SPINDLE_BASE + i * MB_REG_SPINDLE_STRIDE];

        r[0]  = cfg->spindle[i].enabled ? 1 : 0;
        r[1]  = (uint16_t)sp->state;
        r[2]  = sp->monitoring_armed ? 1 : 0;
        r[3]  = scale100(sp->current_a);
        r[4]  = scale100(sp->current_avg_a);
        r[5]  = scale100(sp->current_peak_a);
        r[6]  = scale100(sp->pressure);
        r[7]  = clamp_u16((uint32_t)sp->rpm);
        r[8]  = (uint16_t)sp->severity;
        r[9]  = sp->any_alarm   ? 1 : 0;
        r[10] = sp->any_warning ? 1 : 0;
        r[11] = (uint16_t)sp->current_status;
        r[12] = (uint16_t)sp->pressure_status;
        r[13] = sp->rpm_sensor_suspect ? 1 : 0;
        r[14] = (uint16_t)(sp->cycle_count >> 16);
        r[15] = (uint16_t)(sp->cycle_count & 0xFFFFu);
        r[16] = clamp_u16(sp->last_cycle_ms);
        r[17] = scale100(sp->last_cycle_mean_a);
    }

    /* s_handle_mutex keeps a concurrent reconfigure from deleting these
     * handles out from under us. Both instances share this memory, so also
     * take both instance locks (if running) so neither stack observes a
     * half-written register set mid-request. */
    xSemaphoreTake(s_handle_mutex, portMAX_DELAY);
    if (s_rtu_handle) mbc_slave_lock(s_rtu_handle);
    if (s_tcp_handle) mbc_slave_lock(s_tcp_handle);
    memcpy(s_holding_regs, regs, sizeof(s_holding_regs));
    if (s_tcp_handle) mbc_slave_unlock(s_tcp_handle);
    if (s_rtu_handle) mbc_slave_unlock(s_rtu_handle);
    xSemaphoreGive(s_handle_mutex);
}

static void modbus_task(void *arg)
{
    (void)arg;
    for (;;) {
        monitor_snapshot_t snap;
        monitor_get_snapshot(&snap);
        update_registers(&snap, config_get());
        vTaskDelay(pdMS_TO_TICKS(MB_UPDATE_PERIOD_MS));
    }
}

/* ============================================================
 * Slave instance setup
 * ============================================================ */

static uart_parity_t to_uart_parity(cfg_parity_t p)
{
    switch (p) {
    case CFG_PARITY_ODD:  return UART_PARITY_ODD;
    case CFG_PARITY_EVEN: return UART_PARITY_EVEN;
    default:              return UART_PARITY_DISABLE;
    }
}

static esp_err_t register_descriptor(void *handle)
{
    mb_register_area_descriptor_t reg_area = {
        .type = MB_PARAM_HOLDING,
        .start_offset = 0,
        .address = (void *)s_holding_regs,
        .size = sizeof(s_holding_regs),
        .access = MB_ACCESS_RO,
    };
    return mbc_slave_set_descriptor(handle, reg_area);
}

static esp_err_t start_rtu(const modbus_cfg_t *mcfg)
{
    mb_communication_info_t comm = {0};
    comm.ser_opts.port      = MB_UART_PORT_NUM;
    comm.ser_opts.mode      = MB_RTU;
    comm.ser_opts.baudrate  = mcfg->baud;
    comm.ser_opts.parity    = to_uart_parity(mcfg->parity);
    comm.ser_opts.uid       = mcfg->slave_id;
    comm.ser_opts.data_bits = (mcfg->data_bits == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS;
    comm.ser_opts.stop_bits = (mcfg->stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;

    esp_err_t err = mbc_slave_create_serial(&comm, &s_rtu_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTU slave create failed: %s", esp_err_to_name(err));
        return err;
    }

    err = register_descriptor(s_rtu_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTU register descriptor failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(uart_set_pin(MB_UART_PORT_NUM, PIN_MB_TXD, PIN_MB_RXD,
                                 PIN_MB_DE_RE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(MB_UART_PORT_NUM, UART_MODE_RS485_HALF_DUPLEX));

    err = mbc_slave_start(s_rtu_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTU slave start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Modbus RTU slave up: %lu baud, id %u",
             (unsigned long)mcfg->baud, mcfg->slave_id);
    return ESP_OK;
}

static esp_err_t start_tcp(const modbus_cfg_t *mcfg)
{
    mb_communication_info_t comm = {0};
    comm.tcp_opts.mode          = MB_TCP;
    comm.tcp_opts.port          = mcfg->tcp_port;
    comm.tcp_opts.addr_type     = MB_IPV4;
    comm.tcp_opts.ip_addr_table = NULL;   /* bind all interfaces: AP + STA */
    comm.tcp_opts.ip_netif_ptr  = NULL;
    comm.tcp_opts.uid           = mcfg->slave_id;

    esp_err_t err = mbc_slave_create_tcp(&comm, &s_tcp_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCP slave create failed: %s", esp_err_to_name(err));
        return err;
    }

    err = register_descriptor(s_tcp_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCP register descriptor failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mbc_slave_start(s_tcp_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCP slave start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Modbus TCP slave up: port %u, id %u", mcfg->tcp_port, mcfg->slave_id);
    return ESP_OK;
}

static void stop_all(void)
{
    if (s_rtu_handle) {
        mbc_slave_delete(s_rtu_handle);
        s_rtu_handle = NULL;
    }
    if (s_tcp_handle) {
        mbc_slave_delete(s_tcp_handle);
        s_tcp_handle = NULL;
    }
}

static esp_err_t start_all(const app_config_t *cfg)
{
    const modbus_cfg_t *mcfg = &cfg->system.modbus;
    esp_err_t err;

    if (mcfg->rtu_enabled) {
        err = start_rtu(mcfg);
        if (err != ESP_OK) return err;
    }
    if (mcfg->tcp_enabled) {
        err = start_tcp(mcfg);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t modbus_init(const app_config_t *cfg)
{
    memset(s_holding_regs, 0, sizeof(s_holding_regs));

    s_handle_mutex = xSemaphoreCreateMutex();
    if (!s_handle_mutex) return ESP_ERR_NO_MEM;

    esp_err_t err = start_all(cfg);
    if (err != ESP_OK) {
        stop_all();
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(modbus_task, "modbus", STACK_MODBUS,
                                            NULL, PRIO_WEB, &s_task, CORE_NETWORK);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t modbus_reconfigure(const app_config_t *cfg)
{
    xSemaphoreTake(s_handle_mutex, portMAX_DELAY);
    stop_all();
    esp_err_t err = start_all(cfg);
    xSemaphoreGive(s_handle_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfigure failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Modbus settings reloaded");
    }
    return err;
}
