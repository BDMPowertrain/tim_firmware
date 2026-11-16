/*
 * CRC-32/ISO-HDLC (the zlib and Ethernet CRC): polynomial 0x04C11DB7
 * reflected, initial value and final XOR 0xFFFFFFFF. Check value of
 * "123456789" is 0xCBF43926. Used to seal each stored Parameter copy.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Continues crc over len bytes of data. Start with 0; the result of one call
   is the crc of the next, so a buffer can be fed in pieces. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#endif /* CRC32_H */
