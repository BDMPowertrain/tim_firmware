/* crc16.c — CRC-16/IBM-3740, see crc16.h. Bitwise, no table: an Event is
   32 bytes. */

#include "crc16.h"

uint16_t crc16(const void *data, size_t len)
{
    const uint8_t *bytes = data;
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(bytes[i] << 8);
        for (uint32_t bit = 0; bit < 8u; bit++) {
            crc = (crc & 0x8000u) != 0u ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
