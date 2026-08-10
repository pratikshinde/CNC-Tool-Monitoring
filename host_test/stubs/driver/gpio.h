/*
 * Minimal driver/gpio.h stand-in for the host test build.
 *
 * board.h needs gpio_num_t to declare the pin map. Pulling the real
 * ESP-IDF header into a host build would drag in the whole HAL, so this
 * supplies just the enum. It is only ever seen by gcc on a PC — the
 * firmware build uses the real header.
 */
#pragma once

typedef enum {
    GPIO_NUM_0  = 0,  GPIO_NUM_1  = 1,  GPIO_NUM_2  = 2,  GPIO_NUM_3  = 3,
    GPIO_NUM_4  = 4,  GPIO_NUM_5  = 5,  GPIO_NUM_6  = 6,  GPIO_NUM_7  = 7,
    GPIO_NUM_8  = 8,  GPIO_NUM_9  = 9,  GPIO_NUM_10 = 10, GPIO_NUM_11 = 11,
    GPIO_NUM_12 = 12, GPIO_NUM_13 = 13, GPIO_NUM_14 = 14, GPIO_NUM_15 = 15,
    GPIO_NUM_16 = 16, GPIO_NUM_17 = 17, GPIO_NUM_18 = 18, GPIO_NUM_19 = 19,
    GPIO_NUM_20 = 20, GPIO_NUM_21 = 21, GPIO_NUM_22 = 22, GPIO_NUM_23 = 23,
    GPIO_NUM_25 = 25, GPIO_NUM_26 = 26, GPIO_NUM_27 = 27,
    GPIO_NUM_32 = 32, GPIO_NUM_33 = 33, GPIO_NUM_34 = 34, GPIO_NUM_35 = 35,
    GPIO_NUM_36 = 36, GPIO_NUM_39 = 39,
} gpio_num_t;

/* board.h references I2C_NUM_0 for the port constant. */
#define I2C_NUM_0 0
