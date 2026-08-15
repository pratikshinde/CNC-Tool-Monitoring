/*
 * modbus.c
 *
 * Register map (holding registers, function codes 03/04, read-only).
 * Rebuilt for V2 in the migration's Phase 4 against monitor.h's expanded
 * snapshot (band bitmaps, diagnostics, per-link health) — see
 * MODBUS_REGISTER_MAP.md for the fuller, aspirational design this is
 * scoped down from. Two blocks in that document are deliberately NOT
 * implemented here: the rolling-window statistics and the adaptive-
 * baseline registers (§7). Neither exists anywhere upstream of this file
 * yet — smu_telemetry_t (shared/smu_proto.h) carries no baseline or
 * windowed-average fields at all — so there is nothing to wire them
 * from. Adding them means extending the wire protocol first, which is a
 * separate change from "expose what the SMU already reports".
 *
 * Per FIRMWARE_DESIGN_SPEC.md §5.3's data-quality rule, carried over from
 * V1's map unchanged: process registers are never synthesised from link
 * state. If a spindle's link is stale, its measurement registers below
 * hold the last value the SMU actually reported (monitor.c's translate()
 * already enforces this), and the link block is where staleness itself
 * is reported — a client that ignores the link block and reads only the
 * measurement registers sees old-but-real numbers, never invented ones.
 *
 *   0        schema version of the config that produced these readings
 *              (CONFIG_SCHEMA_VERSION)
 *   1        system_healthy      (0/1) — both links fresh AND no diagnostic
 *   2        adc_healthy         (0/1) — V2 meaning: both SMU links fresh
 *   3        diagnostic_fault    (0/1)
 *   4        loop_period_ms (clamped to 65535)
 *   5        worst_loop_ms  (clamped to 65535)
 *
 *   20 + spindle*10   per-spindle LINK block (spindle 0 -> 20, 1 -> 30)
 *     +0   link_state           (0 down, 1 degraded, 2 live — smu_link_state_t)
 *     +1   have_ever_linked     (0/1) — distinguishes "never seen" from "lost"
 *     +2   link_age_ms          (ms since last good frame, clamped)
 *     +3   crc_error_count      (clamped, cumulative since boot)
 *     +4   timeout_count        (clamped, cumulative since boot)
 *     +5   fw_version           (SMU-reported, (major<<8)|minor)
 *     +6   reset_cause          (smu_ident_t.reset_cause)
 *     +7   reset_count          (cumulative since SMU power-on)
 *
 *   100 + spindle*80  per-spindle TELEMETRY block (spindle 0 -> 100, 1 -> 180)
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
 *     +11  any_latched          (0/1) — something needs acknowledgement
 *     +12  breakage             (0/1)
 *     +13  crash                (0/1)
 *     +14  trend                (0/1)
 *     +15  current_status       (sensor_status_t, scaling.h)
 *     +16  pressure_status      (sensor_status_t)
 *     +17  rpm_sensor_suspect   (0/1)
 *     +18  output_state         (SMU_OUT_* bitmask — the real hard-wired
 *                                 outputs, read back, not recomputed)
 *     +19  diag_flags           (raw SMU_DIAG_* bitmask, smu_proto.h)
 *     +20  bands_active[QTY_CURRENT]   (bit0..3 = LoLo/Lo/Hi/HiHi)
 *     +21  bands_active[QTY_PRESSURE]
 *     +22  bands_active[QTY_RPM]
 *     +23  bands_latched[QTY_CURRENT]
 *     +24  bands_latched[QTY_PRESSURE]
 *     +25  bands_latched[QTY_RPM]
 *     +26  cycle_count hi 16 bits
 *     +27  cycle_count lo 16 bits
 *     +28  last_cycle_ms        (clamped to 65535)
 *     +29  last_cycle_mean_a x100
 *     +30  last_cycle_peak_a x100
 *     +31  pressure_loop_ma x100 (diagnostic — see calib.c)
 *     (+32..+79 reserved — the 80-register stride leaves headroom for the
 *      rolling-window/baseline registers once the wire protocol carries
 *      that data, without another address-space renumbering)
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

#define MB_REG_SYS_BASE        0

#define MB_REG_LINK_BASE       20
#define MB_REG_LINK_STRIDE     10

#define MB_REG_SPINDLE_BASE    100
#define MB_REG_SPINDLE_STRIDE  80

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
    regs[5] = clamp_u16(snap->worst_loop_ms);

    for (int i = 0; i < NUM_SPINDLES; i++) {
        const smu_link_status_t *ls = &snap->link[i];
        uint16_t *l = &regs[MB_REG_LINK_BASE + i * MB_REG_LINK_STRIDE];

        l[0] = (uint16_t)ls->state;
        l[1] = ls->have_ever_linked ? 1 : 0;
        l[2] = clamp_u16(ls->age_ms);
        l[3] = clamp_u16(ls->crc_error_count);
        l[4] = clamp_u16(ls->timeout_count);
        l[5] = ls->fw_version;
        l[6] = ls->reset_cause;
        l[7] = ls->reset_count;

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
        r[9]  = sp->any_alarm    ? 1 : 0;
        r[10] = sp->any_warning  ? 1 : 0;
        r[11] = sp->any_latched  ? 1 : 0;
        r[12] = sp->breakage     ? 1 : 0;
        r[13] = sp->crash        ? 1 : 0;
        r[14] = sp->trend        ? 1 : 0;
        r[15] = (uint16_t)sp->current_status;
        r[16] = (uint16_t)sp->pressure_status;
        r[17] = sp->rpm_sensor_suspect ? 1 : 0;
        r[18] = sp->output_state;
        r[19] = sp->diag_flags;
        r[20] = sp->bands_active[QTY_CURRENT];
        r[21] = sp->bands_active[QTY_PRESSURE];
        r[22] = sp->bands_active[QTY_RPM];
        r[23] = sp->bands_latched[QTY_CURRENT];
        r[24] = sp->bands_latched[QTY_PRESSURE];
        r[25] = sp->bands_latched[QTY_RPM];
        r[26] = (uint16_t)(sp->cycle_count >> 16);
        r[27] = (uint16_t)(sp->cycle_count & 0xFFFFu);
        r[28] = clamp_u16(sp->last_cycle_ms);
        r[29] = scale100(sp->last_cycle_mean_a);
        r[30] = scale100(sp->last_cycle_peak_a);
        r[31] = scale100(sp->pressure_loop_ma);
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
