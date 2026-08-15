/*
 * Minimal esp_err.h stand-in for the host test build.
 *
 * smu_link.c is deliberately free of any other ESP-IDF dependency (see
 * its own header comment) specifically so its protocol logic — CRC and
 * staleness checking, link state transitions, the two-phase config
 * write — can be host-tested against a fake transport, with only this
 * one type crossing the boundary. Values match the real
 * components/esp_common/include/esp_err.h exactly; do not let this
 * drift from that file if it ever changes.
 */
#pragma once

typedef int esp_err_t;

#define ESP_OK                      0
#define ESP_FAIL                    -1

#define ESP_ERR_NO_MEM              0x101
#define ESP_ERR_INVALID_ARG         0x102
#define ESP_ERR_INVALID_STATE       0x103
#define ESP_ERR_INVALID_SIZE        0x104
#define ESP_ERR_NOT_FOUND           0x105
#define ESP_ERR_NOT_SUPPORTED       0x106
#define ESP_ERR_TIMEOUT             0x107
#define ESP_ERR_INVALID_RESPONSE    0x108
#define ESP_ERR_INVALID_CRC         0x109
#define ESP_ERR_INVALID_VERSION     0x10A
#define ESP_ERR_INVALID_MAC         0x10B
#define ESP_ERR_NOT_FINISHED        0x10C
#define ESP_ERR_NOT_ALLOWED         0x10D
