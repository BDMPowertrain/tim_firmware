/*
 * What the VCU control step sends on the Control bus, and when (ADR 0004,
 * ADR 0010). A frame is built from current values on the step it is due and
 * handed straight to a hardware send slot; nothing is queued.
 *
 * Send table: VCU_Status every second step, from the first. The inverter
 * command (0x0C0, every step) joins it when the VCU leaves STARTUP
 * (vcu_control.h).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef VCU_TX_H
#define VCU_TX_H

#include <stdbool.h>
#include <stdint.h>

#include "can_control.h"
#include "e2e_p11.h"

/* step_count is 0 on the first step. */
bool vcu_tx_status_due(uint32_t step_count);

/*
 * Packs the VCU_Status signals (crc and counter are ignored) and protects
 * the frame with E2E Profile 11, advancing tx. frame is
 * CAN_CONTROL_VCU_STATUS_LENGTH bytes.
 */
void vcu_tx_build_status(const struct can_control_vcu_status_t *signals,
                         e2e_p11_tx_t *tx, uint8_t *frame);

#endif /* VCU_TX_H */
