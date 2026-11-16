/*
 * Host tests for the table of received Control bus frames
 * (App/common/logic/Inc/control_rx.h, ADR 0004).
 *
 * The expected timeouts are the spec's (issue 36, "Message database and
 * CAN": BMS_Status 100 ms, BMS_Limits 300 ms, BMS_Power 100 ms, inverter
 * frames 100 ms, Energy Meter 100 ms, iso175 500 ms; "Charge mode": charger
 * 5 s). Each Substitute is unpacked with the generated code and judged by
 * what it tells a reader.
 */
#include <string.h>

#include "can_control.h"
#include "control_rx.h"
#include "e2e_p11.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const mailbox_config_t *row(control_rx_t frame)
{
    return &control_rx_config[frame];
}

static void assert_row(control_rx_t frame, uint32_t id, bool extended, bool e2e,
                       uint32_t timeout_ms)
{
    const mailbox_config_t *config = row(frame);

    TEST_ASSERT_EQUAL_HEX32(id, config->frame_id);
    TEST_ASSERT_EQUAL(extended, config->extended);
    TEST_ASSERT_EQUAL_UINT8(8, config->length);
    TEST_ASSERT_EQUAL(e2e, config->e2e);
    if (e2e) {
        /* The Data ID of our frames is the CAN ID (can/control.dbc). */
        TEST_ASSERT_EQUAL_HEX16(id, config->e2e_data_id);
    }
    TEST_ASSERT_EQUAL_UINT32(timeout_ms, config->timeout_ms);
}

static void test_every_received_frame_has_its_id_checks_and_timeout(void)
{
    assert_row(CONTROL_RX_BMS_STATUS, 0x080, false, true, 100);
    assert_row(CONTROL_RX_BMS_LIMITS, 0x081, false, true, 300);
    assert_row(CONTROL_RX_BMS_POWER, 0x082, false, true, 100);
    assert_row(CONTROL_RX_INV_MOTOR_POSITION, 0x0A5, false, false, 100);
    assert_row(CONTROL_RX_INV_VOLTAGE, 0x0A7, false, false, 100);
    assert_row(CONTROL_RX_INV_INTERNAL_STATES, 0x0AA, false, false, 100);
    assert_row(CONTROL_RX_INV_FAULT_CODES, 0x0AB, false, false, 100);
    assert_row(CONTROL_RX_INV_TORQUE_CAPABILITY, 0x0B1, false, false, 100);
    assert_row(CONTROL_RX_EM_MEASUREMENT, 0x10D, false, false, 100);
    assert_row(CONTROL_RX_EM_STATUS, 0x40D, false, false, 300);
    assert_row(CONTROL_RX_IMD_INFO_GENERAL, 0x18FF01F4u, true, false, 500);
    assert_row(CONTROL_RX_CHG_STATUS, 0x18FF50E5u, true, false, 5000);
    TEST_ASSERT_EQUAL_INT(12, CONTROL_RX_COUNT);
}

static void test_every_timeout_is_at_least_three_periods(void)
{
    static const uint32_t period_ms[CONTROL_RX_COUNT] = {
        [CONTROL_RX_BMS_STATUS] = CAN_CONTROL_BMS_STATUS_CYCLE_TIME_MS,
        [CONTROL_RX_BMS_LIMITS] = CAN_CONTROL_BMS_LIMITS_CYCLE_TIME_MS,
        [CONTROL_RX_BMS_POWER] = CAN_CONTROL_BMS_POWER_CYCLE_TIME_MS,
        [CONTROL_RX_INV_MOTOR_POSITION] = CAN_CONTROL_INV_MOTOR_POSITION_CYCLE_TIME_MS,
        [CONTROL_RX_INV_VOLTAGE] = CAN_CONTROL_INV_VOLTAGE_CYCLE_TIME_MS,
        [CONTROL_RX_INV_INTERNAL_STATES] = CAN_CONTROL_INV_INTERNAL_STATES_CYCLE_TIME_MS,
        [CONTROL_RX_INV_FAULT_CODES] = CAN_CONTROL_INV_FAULT_CODES_CYCLE_TIME_MS,
        [CONTROL_RX_INV_TORQUE_CAPABILITY] = CAN_CONTROL_INV_TORQUE_CAPABILITY_CYCLE_TIME_MS,
        [CONTROL_RX_EM_MEASUREMENT] = CAN_CONTROL_EM_MEASUREMENT_CYCLE_TIME_MS,
        [CONTROL_RX_EM_STATUS] = CAN_CONTROL_EM_STATUS_CYCLE_TIME_MS,
        [CONTROL_RX_IMD_INFO_GENERAL] = CAN_CONTROL_IMD_INFO_GENERAL_CYCLE_TIME_MS,
        [CONTROL_RX_CHG_STATUS] = CAN_CONTROL_CHG_STATUS_CYCLE_TIME_MS,
    };

    for (int i = 0; i < CONTROL_RX_COUNT; i++) {
        TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(0, period_ms[i], "no period");
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(3u * period_ms[i],
                                            control_rx_config[i].timeout_ms);
    }
}

static void test_the_bms_status_substitute_says_bms_faulted(void)
{
    struct can_control_bms_status_t s;

    TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(
        &s, row(CONTROL_RX_BMS_STATUS)->substitute, 8));
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_BMS_STATUS_STATE_FAULT_CHOICE, s.state);
    TEST_ASSERT_EQUAL_UINT8(0, s.bms_ok);
    TEST_ASSERT_EQUAL_UINT8(0, s.shutdown_circuit_closed);
}

static void test_the_bms_limits_and_power_substitutes_allow_no_current(void)
{
    struct can_control_bms_limits_t limits;
    struct can_control_bms_power_t power;

    TEST_ASSERT_EQUAL_INT(0, can_control_bms_limits_unpack(
        &limits, row(CONTROL_RX_BMS_LIMITS)->substitute, 8));
    TEST_ASSERT_EQUAL_UINT16(0, limits.charge_limit);

    TEST_ASSERT_EQUAL_INT(0, can_control_bms_power_unpack(
        &power, row(CONTROL_RX_BMS_POWER)->substitute, 8));
    TEST_ASSERT_EQUAL_UINT16(0, power.discharge_limit);
    TEST_ASSERT_EQUAL_INT16(0, power.pack_current);
    TEST_ASSERT_EQUAL_UINT16(0, power.pack_voltage);
}

static void test_the_inverter_substitutes_say_inverter_not_ready(void)
{
    struct can_control_inv_internal_states_t states;
    struct can_control_inv_voltage_t voltage;
    struct can_control_inv_motor_position_t position;
    struct can_control_inv_fault_codes_t faults;
    struct can_control_inv_torque_capability_t capability;

    TEST_ASSERT_EQUAL_INT(0, can_control_inv_internal_states_unpack(
        &states, row(CONTROL_RX_INV_INTERNAL_STATES)->substitute, 8));
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_INV_INTERNAL_STATES_VSM_STATE_START_CHOICE,
                            states.vsm_state);
    TEST_ASSERT_EQUAL_UINT8(0, states.enable_state);

    TEST_ASSERT_EQUAL_INT(0, can_control_inv_voltage_unpack(
        &voltage, row(CONTROL_RX_INV_VOLTAGE)->substitute, 8));
    TEST_ASSERT_EQUAL_INT16(0, voltage.dc_bus_voltage);

    TEST_ASSERT_EQUAL_INT(0, can_control_inv_motor_position_unpack(
        &position, row(CONTROL_RX_INV_MOTOR_POSITION)->substitute, 8));
    TEST_ASSERT_EQUAL_INT16(0, position.motor_speed);

    /* Silence is not a fault the inverter reported. */
    TEST_ASSERT_EQUAL_INT(0, can_control_inv_fault_codes_unpack(
        &faults, row(CONTROL_RX_INV_FAULT_CODES)->substitute, 8));
    TEST_ASSERT_EQUAL_HEX32(0, faults.post_faults);
    TEST_ASSERT_EQUAL_HEX32(0, faults.run_faults);

    TEST_ASSERT_EQUAL_INT(0, can_control_inv_torque_capability_unpack(
        &capability, row(CONTROL_RX_INV_TORQUE_CAPABILITY)->substitute, 8));
    TEST_ASSERT_EQUAL_INT16(0, capability.motor_torque_available);
}

static void test_the_reported_only_substitutes_claim_nothing(void)
{
    struct can_control_em_measurement_t measurement;
    struct can_control_em_status_t status;
    struct can_control_imd_info_general_t imd;

    TEST_ASSERT_EQUAL_INT(0, can_control_em_measurement_unpack(
        &measurement, row(CONTROL_RX_EM_MEASUREMENT)->substitute, 8));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, measurement.current);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, measurement.voltage);

    TEST_ASSERT_EQUAL_INT(0, can_control_em_status_unpack(
        &status, row(CONTROL_RX_EM_STATUS)->substitute, 8));
    TEST_ASSERT_EQUAL_UINT8(0, status.violation);

    /* The iso175's own "not available" values, and no alarm. */
    TEST_ASSERT_EQUAL_INT(0, can_control_imd_info_general_unpack(
        &imd, row(CONTROL_RX_IMD_INFO_GENERAL)->substitute, 8));
    TEST_ASSERT_EQUAL_UINT16(65535, imd.r_iso_corrected);
    TEST_ASSERT_EQUAL_UINT8(
        CAN_CONTROL_IMD_INFO_GENERAL_R_ISO_STATUS_NOT_AVAILABLE_CHOICE, imd.r_iso_status);
    TEST_ASSERT_EQUAL_UINT8(0, imd.iso_alarm);
    TEST_ASSERT_EQUAL_UINT8(0, imd.unsafe_to_start);
}

static void test_the_charger_substitute_is_a_charger_putting_out_nothing(void)
{
    struct can_control_chg_status_t status;

    TEST_ASSERT_EQUAL_INT(0, can_control_chg_status_unpack(
        &status, row(CONTROL_RX_CHG_STATUS)->substitute, 8));
    TEST_ASSERT_EQUAL_UINT16(0, status.output_voltage);
    TEST_ASSERT_EQUAL_UINT16(0, status.output_current);
    TEST_ASSERT_EQUAL_UINT8(0, status.hardware_failure);
}

/* A BMS_Status as the BMS sends it: DRIVE, BMS_OK high, Shutdown Circuit closed. */
static void send_healthy_bms_status(e2e_p11_tx_t *sender, mailbox_rx_t *rx, uint32_t at)
{
    struct can_control_bms_status_t s;
    uint8_t frame[8];

    memset(&s, 0, sizeof s);
    s.state = CAN_CONTROL_BMS_STATUS_STATE_DRIVE_CHOICE;
    s.bms_ok = 1;
    s.shutdown_circuit_closed = 1;
    TEST_ASSERT_EQUAL_INT(8, can_control_bms_status_pack(frame, &s, sizeof frame));
    e2e_p11_protect(sender, 0x0080u, frame, sizeof frame);
    mailbox_rx_store(rx, frame, sizeof frame, at);
}

/* The whole path: generated pack, E2E, mailbox, table, generated unpack. */
static void test_a_reader_of_bms_status_gets_the_frame_while_valid_and_bms_faulted_after_100_ms(void)
{
    const mailbox_config_t *config = row(CONTROL_RX_BMS_STATUS);
    e2e_p11_tx_t sender = {0};
    mailbox_rx_t rx;
    mailbox_t mb;
    struct can_control_bms_status_t read;

    memset(&rx, 0, sizeof rx);
    memset(&mb, 0, sizeof mb);

    send_healthy_bms_status(&sender, &rx, 1000);
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, mailbox_update(&mb, config, &rx, 1000));
    send_healthy_bms_status(&sender, &rx, 1020);
    TEST_ASSERT_EQUAL(MAILBOX_NODATA, mailbox_update(&mb, config, &rx, 1020));

    /* Two healthy frames are not enough: the reader still sees a faulted BMS. */
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(
        &read, mailbox_read(&mb, config), config->length));
    TEST_ASSERT_EQUAL_UINT8(0, read.bms_ok);

    send_healthy_bms_status(&sender, &rx, 1040);
    TEST_ASSERT_EQUAL(MAILBOX_VALID, mailbox_update(&mb, config, &rx, 1040));
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(
        &read, mailbox_read(&mb, config), config->length));
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_BMS_STATUS_STATE_DRIVE_CHOICE, read.state);
    TEST_ASSERT_EQUAL_UINT8(1, read.bms_ok);

    TEST_ASSERT_EQUAL(MAILBOX_VALID, mailbox_update(&mb, config, &rx, 1139));
    TEST_ASSERT_EQUAL(MAILBOX_INVALID, mailbox_update(&mb, config, &rx, 1140));
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(
        &read, mailbox_read(&mb, config), config->length));
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_BMS_STATUS_STATE_FAULT_CHOICE, read.state);
    TEST_ASSERT_EQUAL_UINT8(0, read.bms_ok);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_received_frame_has_its_id_checks_and_timeout);
    RUN_TEST(test_every_timeout_is_at_least_three_periods);
    RUN_TEST(test_the_bms_status_substitute_says_bms_faulted);
    RUN_TEST(test_the_bms_limits_and_power_substitutes_allow_no_current);
    RUN_TEST(test_the_inverter_substitutes_say_inverter_not_ready);
    RUN_TEST(test_the_reported_only_substitutes_claim_nothing);
    RUN_TEST(test_the_charger_substitute_is_a_charger_putting_out_nothing);
    RUN_TEST(test_a_reader_of_bms_status_gets_the_frame_while_valid_and_bms_faulted_after_100_ms);
    return UNITY_END();
}
