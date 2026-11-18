/*
 * What the BMS control step sends on the Control bus, and when (ADR 0004,
 * ADR 0010). A frame is built from current values on the step it is due and
 * handed straight to a hardware send slot; nothing is queued.
 *
 * Send table (spec #36; issue 66), a five-step cycle: BMS_Status and
 * BMS_Power every step, from the first, plus one slower frame a step in
 * the order of bms_tx_slow_t, so each goes every 100 ms. That is three
 * frames a step, never more: the controller has three send slots and a
 * frame never waits for one (ADR 0004). 0x60D, every 100 ms, is inside the
 * Energy Meter's 250 ms. At the step count's wrap (after 994 days) one slot
 * repeats, delaying the others by one step.
 * In Charge mode the charger request takes the 0x202 slot on one step in
 * fifty (#67).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BMS_TX_H
#define BMS_TX_H

#include <stdbool.h>
#include <stdint.h>

#include "can_control.h"
#include "e2e_p11.h"

/* At most this many frames are due on a step: the hardware send slots. */
#define BMS_TX_FRAMES_MAX 3u

/* One frame built by the step, for control_bus_send(). */
typedef struct {
    uint32_t id;
    bool     extended;
    uint8_t  len;
    uint8_t  data[8];
} bms_tx_frame_t;

/* The slower frames, one a step, in this order. */
typedef enum {
    BMS_TX_SLOW_LIMITS,          /* BMS_Limits (0x081) */
    BMS_TX_SLOW_INVERTER_LIMITS, /* INV_BmsLimits (0x202) */
    BMS_TX_SLOW_EM_CELL_TEMPS,   /* EM_CellTemps (0x60D) */
    BMS_TX_SLOW_PACK_VOLTAGES,   /* BMS_PackVoltages (0x210) */
    BMS_TX_SLOW_PACK_TEMPS,      /* BMS_PackTemps (0x211) */
    BMS_TX_SLOW_COUNT,
    BMS_TX_SLOW_NONE = BMS_TX_SLOW_COUNT
} bms_tx_slow_t;

/* step_count is 0 on the first step. */
bool bms_tx_status_due(uint32_t step_count);
bool bms_tx_power_due(uint32_t step_count);
bms_tx_slow_t bms_tx_slow_due(uint32_t step_count);

/*
 * Packs the BMS_Status signals (crc and counter are ignored) and protects
 * the frame with E2E Profile 11, advancing tx. frame is
 * CAN_CONTROL_BMS_STATUS_LENGTH bytes.
 */
void bms_tx_build_status(const struct can_control_bms_status_t *signals,
                         e2e_p11_tx_t *tx, uint8_t *frame);

/* As bms_tx_build_status(), for BMS_Power (0x082) and BMS_Limits (0x081). */
void bms_tx_build_power(const struct can_control_bms_power_t *signals,
                        e2e_p11_tx_t *tx, uint8_t *frame);
void bms_tx_build_limits(const struct can_control_bms_limits_t *signals,
                         e2e_p11_tx_t *tx, uint8_t *frame);

#endif /* BMS_TX_H */
