#include "vcu_tx.h"

#define VCU_STATUS_EVERY_STEPS 2u /* 20 ms on the 10 ms step */

bool vcu_tx_status_due(uint32_t step_count)
{
    return (step_count % VCU_STATUS_EVERY_STEPS) == 0u;
}

void vcu_tx_build_status(const struct can_control_vcu_status_t *signals,
                         e2e_p11_tx_t *tx, uint8_t *frame)
{
    (void)can_control_vcu_status_pack(frame, signals,
                                      CAN_CONTROL_VCU_STATUS_LENGTH);
    /* The Data ID is the CAN ID (can/control.dbc). */
    e2e_p11_protect(tx, (uint16_t)CAN_CONTROL_VCU_STATUS_FRAME_ID, frame,
                    CAN_CONTROL_VCU_STATUS_LENGTH);
}
