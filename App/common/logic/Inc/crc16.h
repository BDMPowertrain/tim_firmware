/*
 * CRC-16/IBM-3740 (often called CRC-16/CCITT-FALSE): polynomial 0x1021,
 * not reflected, initial value 0xFFFF, no final XOR. Check value of
 * "123456789" is 0x29B1. Used to seal each Event and Event log page header.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

uint16_t crc16(const void *data, size_t len);

#endif /* CRC16_H */
