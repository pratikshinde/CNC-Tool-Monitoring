/*
 * Minimal driver/uart.h stand-in for the host test build.
 *
 * board.h needs UART_NUM_2 to declare the Modbus RS485 port. Pulling the
 * real ESP-IDF header into a host build would drag in the whole HAL, so
 * this supplies just the constant. It is only ever seen by gcc on a PC —
 * the firmware build uses the real header.
 */
#pragma once

#define UART_NUM_2 2
