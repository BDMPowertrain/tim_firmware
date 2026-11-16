/* crc32.c — CRC-32/ISO-HDLC, see crc32.h. Bitwise, no table: it runs only
   on a Parameter load or save, a few KB at a time. */

#include "crc32.h"

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *bytes = data;

    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (uint32_t bit = 0; bit < 8u; bit++) {
            crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }
    return ~crc;
}
