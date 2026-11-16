/*
 * What the BMS control step sends on the Control bus, and when (ADR 0004,
 * ADR 0010). A frame is built from current values on the step it is due and
 * handed straight to a hardware send slot; nothing is queued.
 *
 * Send table: BMS_Status every step, from the first. BMS_Power and the
 * five-step cycle of slower frames join it with the values they carry.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_TX_H
#define BMS_TX_H

#include <stdbool.h>
#include <stdint.h>

#include "can_control.h"
#include "e2e_p11.h"

/* step_count is 0 on the first step. */
bool bms_tx_status_due(uint32_t step_count);

/*
 * Packs the BMS_Status signals (crc and counter are ignored) and protects
 * the frame with E2E Profile 11, advancing tx. frame is
 * CAN_CONTROL_BMS_STATUS_LENGTH bytes.
 */
void bms_tx_build_status(const struct can_control_bms_status_t *signals,
                         e2e_p11_tx_t *tx, uint8_t *frame);

#endif /* BMS_TX_H */
