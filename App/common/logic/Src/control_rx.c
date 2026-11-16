#include "control_rx.h"

#include "can_control.h"

/* A third-party frame: a length check and a timeout, no CRC or counter. */
#define PLAIN(NAME, TIMEOUT_MS, ...)                           \
    {                                                          \
        .frame_id = CAN_CONTROL_##NAME##_FRAME_ID,             \
        .extended = CAN_CONTROL_##NAME##_IS_EXTENDED,          \
        .length = CAN_CONTROL_##NAME##_LENGTH,                 \
        .timeout_ms = (TIMEOUT_MS),                            \
        .substitute = __VA_ARGS__,                             \
    }

/* One of our own frames: E2E Profile 11, and its Data ID is its CAN ID
 * (can/control.dbc). */
#define OURS(NAME, TIMEOUT_MS, ...)                            \
    {                                                          \
        .frame_id = CAN_CONTROL_##NAME##_FRAME_ID,             \
        .extended = CAN_CONTROL_##NAME##_IS_EXTENDED,          \
        .length = CAN_CONTROL_##NAME##_LENGTH,                 \
        .e2e = true,                                           \
        .e2e_data_id = (uint16_t)CAN_CONTROL_##NAME##_FRAME_ID, \
        .timeout_ms = (TIMEOUT_MS),                            \
        .substitute = __VA_ARGS__,                             \
    }

/* A Substitute in which every signal is 0. */
#define ALL_ZERO {0}

/*
 * Timeouts and Substitutes. The values are the spec's (issue 36, "Message
 * database and CAN") except BMS_Limits and the two marked "derived", which
 * the spec does not give. Every timeout is at least three periods.
 *
 * A Substitute is written as frame bytes so that a reader unpacks it exactly
 * as it unpacks a real frame; tests/host/common/test_control_rx.c unpacks
 * each one and checks what it says.
 */
const mailbox_config_t control_rx_config[CONTROL_RX_COUNT] = {
    /* "BMS faulted": state FAULT (byte 1, bits 4-7 = 3), BMS_OK low, no
     * fault bit claimed. */
    [CONTROL_RX_BMS_STATUS] = OURS(BMS_STATUS, 100u,
        {0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),

    /* Charge limit 0 A. 500 ms, not the 300 ms first decided (issue 12):
     * the frame after a lost one is dropped for its skipped counter, so two
     * lost frames leave 400 ms between good ones. Nothing the driving car
     * reacts to quickly is in this frame; the Discharge limit is in
     * BMS_Power. */
    [CONTROL_RX_BMS_LIMITS] = OURS(BMS_LIMITS, 500u, ALL_ZERO),

    /* Discharge limit 0 A, so zero torque. */
    [CONTROL_RX_BMS_POWER] = OURS(BMS_POWER, 100u, ALL_ZERO),

    /* The inverter frames: "inverter not ready". VSM state START, not
     * enabled, 0 V on the bus, 0 rpm, no torque available, no fault claimed. */
    [CONTROL_RX_INV_MOTOR_POSITION] = PLAIN(INV_MOTOR_POSITION, 100u, ALL_ZERO),
    [CONTROL_RX_INV_VOLTAGE] = PLAIN(INV_VOLTAGE, 100u, ALL_ZERO),
    [CONTROL_RX_INV_INTERNAL_STATES] = PLAIN(INV_INTERNAL_STATES, 100u, ALL_ZERO),
    [CONTROL_RX_INV_FAULT_CODES] = PLAIN(INV_FAULT_CODES, 100u, ALL_ZERO),
    [CONTROL_RX_INV_TORQUE_CAPABILITY] = PLAIN(INV_TORQUE_CAPABILITY, 100u, ALL_ZERO),

    /* Energy Meter: reported only, nothing reads it to decide. 0 A, 0 V. */
    [CONTROL_RX_EM_MEASUREMENT] = PLAIN(EM_MEASUREMENT, 100u, ALL_ZERO),

    /* Derived: three 100 ms periods. No Violation, so no Event is written
     * for a meter that is only silent. */
    [CONTROL_RX_EM_STATUS] = PLAIN(EM_STATUS, 300u, ALL_ZERO),

    /* iso175: logged only. Resistance 65535 and status 255 are the monitor's
     * own "not available" values (manual p.9-10); no alarm claimed. */
    [CONTROL_RX_IMD_INFO_GENERAL] = PLAIN(IMD_INFO_GENERAL, 500u,
        {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00}),

    /* Charger: heard within 5 s, or not heard (spec, "Charge mode").
     * Derived Substitute: 0 V, 0 A, no flags, because no charger flag is a
     * BMS fault and 0 A can never count as "charger disobeys". */
    [CONTROL_RX_CHG_STATUS] = PLAIN(CHG_STATUS, 5000u, ALL_ZERO),
};
