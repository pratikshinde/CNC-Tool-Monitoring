/*
 * board.h — physical pin map and fixed hardware constants for the ESP32
 * MASTER board.
 *
 * Everything here describes the ESP32's own PCB, not user configuration
 * (that lives in app_config.h) and not the SMU boards, which have their
 * own pin map — see SMU_HARDWARE_REQUIREMENTS.md §6.1 — compiled into an
 * entirely separate firmware image.
 *
 * Board: CNC Tool Monitor V2 — ESP32 master + 2x SMU (Nuvoton M2003FC1AE),
 * 2 spindles. V1's single-board ESP32 + ADS1115 design is superseded; see
 * FIRMWARE_DESIGN_SPEC.md §1 for why.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
/* I2C_NUM_0 / I2C_NUM_1 (i2c_port_t) resolve transitively through
 * driver/gpio.h's own includes, exactly as they did in V1's board.h,
 * which used I2C_NUM_0 the same bare way with no explicit I2C header —
 * proven by that file actually building. Not adding driver/i2c.h (or
 * the newer driver/i2c_master.h) here deliberately, to avoid pulling in
 * a driver dependency this header doesn't otherwise need; smu_link.c
 * (V2 migration Phase 3) includes whatever the actual I2C driver calls
 * require in its own .c file. */

/* ============================================================
 * Channel counts
 * ============================================================ */

#define NUM_SPINDLES        2

/* ============================================================
 * I2C — one independent bus per SMU (FIRMWARE_DESIGN_SPEC.md §5.1,
 * SMU_HARDWARE_REQUIREMENTS.md §3.1). Deliberately NOT a shared
 * multi-drop bus: a wedged or glitching SMU on one bus must not be able
 * to block telemetry from the other spindle.
 *
 * SMU 1's bus reuses the pins the ADS1115 occupied (GPIO21/22) — that
 * silicon is gone, and I2C_NUM_0's usual pins were already free the
 * moment it was removed. SMU 2's bus is drawn from the GPIO that freed
 * up when the ESP32's own digital outputs (V1's DO0/DO1, dio.c/do_map.c)
 * were retired in V2 — the ESP32 drives no physical output any more, so
 * those pins had no other claim on them. GPIO25/26 chosen for adjacency;
 * not otherwise load-bearing, and free to move if PCB layout wants
 * different pins.
 * ============================================================ */

#define SMU1_I2C_PORT       I2C_NUM_0
#define SMU1_I2C_SDA        GPIO_NUM_21
#define SMU1_I2C_SCL        GPIO_NUM_22

#define SMU2_I2C_PORT       I2C_NUM_1
#define SMU2_I2C_SDA        GPIO_NUM_25   /* was V1's PIN_DO0, now free */
#define SMU2_I2C_SCL        GPIO_NUM_26   /* was V1's PIN_DO1, now free */

#define SMU_I2C_FREQ_HZ     100000   /* standard mode; see FIRMWARE_DESIGN_SPEC.md §5.1 for the 20 Hz poll / bus-utilisation math this is sized against */

/* ============================================================
 * Modbus RTU — RS485 transceiver on UART2
 *
 * Matches the old Arduino sketch's Serial2 wiring (RX=16, TX=17,
 * DE/RE=4); none of these pins are used elsewhere in this pin map.
 * ============================================================ */

#define MB_UART_PORT_NUM    UART_NUM_2
#define PIN_MB_RXD          GPIO_NUM_16
#define PIN_MB_TXD          GPIO_NUM_17
#define PIN_MB_DE_RE        GPIO_NUM_4    /* half-duplex direction control */

/* ============================================================
 * Local indication
 * ============================================================ */

#define PIN_LED_STATUS      GPIO_NUM_2
#define PIN_BTN_RESET       GPIO_NUM_0    /* also the boot strap pin */

/* ============================================================
 * Analogue front-end constants
 *
 * MOVED to components/smu_shared/scaling.h during the V2 migration — see
 * that header for the full rationale and current values. These describe
 * the SMU's analogue front end (SMU_HARDWARE_REQUIREMENTS.md §3.2), not
 * anything on the ESP32 board itself, so they no longer belong here.
 * (V1's PRESSURE_BURDEN_OHM was 180 — a documented defect, now corrected
 * to 100 in scaling.h; do not resurrect the old value if this comment is
 * ever consulted for a "what changed" question.)
 * ============================================================ */

/* ============================================================
 * Task layout (SRS SM-R6)
 *
 * Measurement and alarm evaluation own core 0 so that Wi-Fi and the HTTP
 * server on core 1 can never delay an alarm output. This is the mechanism
 * behind the "UI activity must not affect latency" requirement.
 * ============================================================ */

#define CORE_REALTIME       0
#define CORE_NETWORK        1

#define PRIO_MEASURE        18
#define PRIO_ALARM          19   /* above measure: evaluate as soon as data lands */
#define PRIO_LOGGER          5
#define PRIO_WEB             4

#define STACK_MEASURE       4096
#define STACK_ALARM         4096
#define STACK_LOGGER        4096
#define STACK_MODBUS        4096
/* The config handlers hold a whole app_config_t working copy (~1 KB) on
 * the stack while cJSON recurses over the request body, so the HTTP task
 * needs noticeably more headroom than the other network tasks. */
#define STACK_WEB           8192
