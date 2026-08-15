/*
 * smu_crc16.c — CRC-16/CCITT-FALSE, as specified by smu_proto.h.
 *
 * Poly 0x1021, init 0xFFFF, no input/output reflection, xorout 0x0000.
 * Bit-wise implementation deliberately, not a lookup table: the frames on
 * this link are at most 296 bytes (the config block) and cross the wire at
 * most a few times a second, so the O(1) table's memory cost buys nothing
 * measurable and this is the version that is trivially checked by eye
 * against the algorithm's definition.
 *
 * Reference vector (the standard CRC-16/CCITT-FALSE check value):
 *   smu_crc16("123456789", 9) == 0x29B1
 * Verified in host_test/test_main.c — check that first if this is ever
 * touched.
 */
#include "smu_proto.h"

uint16_t smu_crc16(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                   : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
