/*
 * board.h — physical pin map and fixed hardware constants.
 *
 * Everything here describes the PCB, not user configuration. Anything an
 * operator or commissioning engineer can change lives in app_config.h.
 *
 * Board: CNC Tool Monitor rev A — ESP32 + ADS1115, 2 spindles.
 */
#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"

/* ============================================================
 * Channel counts (SRS §2.1)
 * ============================================================ */

#define NUM_SPINDLES        2
#define NUM_CURRENT_CH      2   /* one CT per spindle          */
#define NUM_PRESSURE_CH     2   /* one transmitter per spindle */
#define NUM_RPM_CH          2   /* DI0, DI1                    */
#define NUM_DIGITAL_IN      4   /* DI2, DI3 fitted but unassigned in v1 */
#define NUM_DIGITAL_OUT     4

/* ============================================================
 * I2C — ADS1115 and (optional) DS3231 RTC share the bus
 * ============================================================ */

#define I2C_PORT_NUM        I2C_NUM_0
#define PIN_I2C_SDA         GPIO_NUM_21
#define PIN_I2C_SCL         GPIO_NUM_22
#define I2C_FREQ_HZ         400000

#define ADS1115_I2C_ADDR    0x48    /* ADDR tied to GND */
#define DS3231_I2C_ADDR     0x68

/* ADS1115 analogue input assignment.
 * Fixed by the PCB: swapping these means cutting tracks, so they are not
 * user-configurable. */
#define ADS_CH_CURRENT_S1   0   /* AIN0 */
#define ADS_CH_CURRENT_S2   1   /* AIN1 */
#define ADS_CH_PRESSURE_S1  2   /* AIN2 */
#define ADS_CH_PRESSURE_S2  3   /* AIN3 */

/* ============================================================
 * Digital inputs — opto-isolated, active low at the MCU
 *
 * The opto output pulls the MCU pin down when field current flows, so a
 * live 24 V input reads 0. Every consumer of these pins must invert.
 * ============================================================ */

#define PIN_DI0             GPIO_NUM_34   /* spindle 1 RPM pulse  */
#define PIN_DI1             GPIO_NUM_35   /* spindle 2 RPM pulse  */
#define PIN_DI2             GPIO_NUM_32   /* reserved (SRS §2.2)  */
#define PIN_DI3             GPIO_NUM_33   /* reserved             */

#define DI_ACTIVE_LEVEL     0             /* opto pulls low when energised */

/* GPIO34/35 are input-only on the ESP32 and have no internal pull-ups.
 * External 10k pull-ups to 3V3 are required on the PCB — without them the
 * pins float when the opto is off and the pulse counter will pick up noise.
 * Asserted here so a future pin reassignment cannot silently break this. */
#if (PIN_DI0 >= GPIO_NUM_34) || (PIN_DI1 >= GPIO_NUM_34)
#  define DI_NEEDS_EXTERNAL_PULLUP 1
#endif

/* ============================================================
 * Digital outputs — drive opto/relay stages to the PLC
 * ============================================================ */

#define PIN_DO0             GPIO_NUM_25
#define PIN_DO1             GPIO_NUM_26
#define PIN_DO2             GPIO_NUM_27
#define PIN_DO3             GPIO_NUM_14

/* Level at the MCU that energises the output stage. */
#define DO_ACTIVE_LEVEL     1

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
 * These describe the fitted passives. Changing a burden resistor on the
 * board means changing the value here — they are not in NVS because a
 * mismatch between the two is a calibration bug, not a setting.
 * ============================================================ */

/* Pressure: 4-20 mA burden resistor. 100R gives 0.4-2.0 V, which sits
 * inside the +/-2.048 V ADS1115 range with headroom for over-range
 * detection up to ~20.4 mA. */
#define PRESSURE_BURDEN_OHM         100.0f

/* Pressure: 0-10 V divider, R1 = 80.6k (top), R2 = 20k (bottom).
 * 10 V in -> 1.985 V at the ADC. */
#define PRESSURE_DIV_R_TOP_OHM      80600.0f
#define PRESSURE_DIV_R_BOT_OHM      20000.0f
#define PRESSURE_DIV_RATIO \
    (PRESSURE_DIV_R_BOT_OHM / (PRESSURE_DIV_R_TOP_OHM + PRESSURE_DIV_R_BOT_OHM))

/* Current: CT burden resistor and the mid-rail bias applied to centre the
 * AC waveform inside the ADC's unipolar window. */
#define CT_BURDEN_OHM               33.0f
#define CT_BIAS_VOLTS               1.024f   /* nominal; measured at auto-zero */

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
#define STACK_WEB           4096
