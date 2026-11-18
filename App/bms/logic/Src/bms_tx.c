#include "bms_tx.h"

bool bms_tx_status_due(uint32_t step_count)
{
    (void)step_count;
    return true;
}

bool bms_tx_power_due(uint32_t step_count)
{
    (void)step_count;
    return true;
}

bms_tx_slow_t bms_tx_slow_due(uint32_t step_count)
{
    return (bms_tx_slow_t)(step_count % (uint32_t)BMS_TX_SLOW_COUNT);
}

void bms_tx_build_status(const struct can_control_bms_status_t *signals,
                         e2e_p11_tx_t *tx, uint8_t *frame)
{
    (void)can_control_bms_status_pack(frame, signals,
                                      CAN_CONTROL_BMS_STATUS_LENGTH);
    /* The Data ID is the CAN ID (can/control.dbc). */
    e2e_p11_protect(tx, (uint16_t)CAN_CONTROL_BMS_STATUS_FRAME_ID, frame,
                    CAN_CONTROL_BMS_STATUS_LENGTH);
}

void bms_tx_build_power(const struct can_control_bms_power_t *signals,
                        e2e_p11_tx_t *tx, uint8_t *frame)
{
    (void)can_control_bms_power_pack(frame, signals, CAN_CONTROL_BMS_POWER_LENGTH);
    e2e_p11_protect(tx, (uint16_t)CAN_CONTROL_BMS_POWER_FRAME_ID, frame,
                    CAN_CONTROL_BMS_POWER_LENGTH);
}

void bms_tx_build_limits(const struct can_control_bms_limits_t *signals,
                         e2e_p11_tx_t *tx, uint8_t *frame)
{
    (void)can_control_bms_limits_pack(frame, signals, CAN_CONTROL_BMS_LIMITS_LENGTH);
    e2e_p11_protect(tx, (uint16_t)CAN_CONTROL_BMS_LIMITS_FRAME_ID, frame,
                    CAN_CONTROL_BMS_LIMITS_LENGTH);
}
