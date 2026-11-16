/*
 * One CAN frame, classic or FD, as the DAQ bus driver queues it.
 *
 * Deliberately compact (~72 B) rather than HAL's FDCAN header types, which
 * carry fields (timestamp, filter index, event-FIFO control) the driver does
 * not use; every byte here is paid for once per queue slot.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef CAN_FRAME_H
#define CAN_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define CAN_FRAME_DATA_MAX 64u

typedef struct {
    uint32_t id;
    uint8_t  len;          /* real byte count: 0-8 classic; 12/16/20/24/32/48/64 for FD */
    bool     is_extended;
    bool     is_fd;
    bool     is_brs;       /* bit-rate switching; only meaningful if is_fd */
    uint8_t  data[CAN_FRAME_DATA_MAX];
} can_frame_t;

#endif /* CAN_FRAME_H */
