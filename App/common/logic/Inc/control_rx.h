/*
 * Every frame an image receives on the Control bus, with its timeout and its
 * Substitute: the one table of them (ADR 0004). A received frame is added
 * here and nowhere else.
 *
 * One mailbox per row. An image keeps mailboxes only for the rows it reads
 * (the receivers are in can/control.dbc).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef CONTROL_RX_H
#define CONTROL_RX_H

#include "mailbox.h"

typedef enum {
    CONTROL_RX_BMS_STATUS,
    CONTROL_RX_BMS_LIMITS,
    CONTROL_RX_BMS_POWER,
    CONTROL_RX_INV_MOTOR_POSITION,
    CONTROL_RX_INV_VOLTAGE,
    CONTROL_RX_INV_INTERNAL_STATES,
    CONTROL_RX_INV_FAULT_CODES,
    CONTROL_RX_INV_TORQUE_CAPABILITY,
    CONTROL_RX_EM_MEASUREMENT,
    CONTROL_RX_EM_STATUS,
    CONTROL_RX_IMD_INFO_GENERAL,
    CONTROL_RX_CHG_STATUS,
    CONTROL_RX_COUNT
} control_rx_t;

/* Indexed by control_rx_t. Unpack mailbox_read() with the frame's
 * can_control_..._unpack() function. */
extern const mailbox_config_t control_rx_config[CONTROL_RX_COUNT];

/*
 * The receive interrupt's lookup: the position in rows (the frames an image
 * reads, one mailbox each) of the frame with this ID, or -1 if the image
 * does not read it. A standard and an extended ID never match each other.
 */
int control_rx_find(const control_rx_t *rows, size_t row_count,
                    uint32_t frame_id, bool extended);

#endif /* CONTROL_RX_H */
