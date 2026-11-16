/*
 * Host tests for the Message database (ADR 0003): the C that cantools
 * generates from can/control.dbc and can/daq.dbc, in
 * App/common/logic/can_db/.
 *
 * At least one frame per sender on each bus goes through pack and unpack.
 * The expected bytes are worked out by hand from the layout written in the
 * DBC comments (and, for third-party frames, from the manual page cited
 * there), so a signal that moves in the DBC fails here.
 *
 * Structs are cleared with memset, not "= {0}", because whole structs are
 * compared as memory and that includes their padding.
 */
#include <string.h>

#include "can_control.h"
#include "can_daq.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Control bus, sender BMS: BMS_Status (0x080). */
static void test_bms_status_packs_to_the_documented_layout_and_unpacks(void)
{
    struct can_control_bms_status_t in;
    struct can_control_bms_status_t out;
    uint8_t frame[8];
    /* byte 0 CRC; byte 1 counter (low half) and state (high half);
     * bytes 2-3 active bits; bytes 4-5 seen bits; byte 6 flags;
     * byte 7 Control bus bits. */
    const uint8_t expected[8] = {0xAB, 0x25, 0x01, 0x01, 0x82, 0x00, 0x05, 0x02};

    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.crc = 0xAB;
    in.counter = 5;
    in.state = CAN_CONTROL_BMS_STATUS_STATE_CHARGE_CHOICE;
    in.over_voltage_active = 1;     /* active bit 0 */
    in.charger_disobeys_active = 1; /* active bit 8 */
    in.under_voltage_seen = 1;      /* seen bit 1 */
    in.internal_seen = 1;           /* seen bit 7 */
    in.bms_ok = 1;                  /* flag bit 0 */
    in.shutdown_circuit_closed = 1; /* flag bit 2 */
    in.error_passive = 1;           /* Control bus bit 1 */

    TEST_ASSERT_EQUAL_INT(8, can_control_bms_status_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);

    TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_HEX32(0x080, CAN_CONTROL_BMS_STATUS_FRAME_ID);
    TEST_ASSERT_EQUAL_UINT(20, CAN_CONTROL_BMS_STATUS_CYCLE_TIME_MS);
}

/* The generated C carries raw values: BMS_Power is in 0.1 A and 0.1 V. */
static void test_bms_power_carries_current_and_voltage_in_tenths(void)
{
    struct can_control_bms_power_t in;
    struct can_control_bms_power_t out;
    uint8_t frame[8];
    /* -123.4 A is -1234 = 0xFB2E; 412.5 V is 4125 = 0x101D; 200.0 A is
     * 2000 = 0x07D0. Little-endian. */
    const uint8_t expected[8] = {0x00, 0x03, 0x2E, 0xFB, 0x1D, 0x10, 0xD0, 0x07};

    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.counter = 3;
    in.pack_current = -1234;
    in.pack_voltage = 4125;
    in.discharge_limit = 2000;

    TEST_ASSERT_EQUAL_INT(8, can_control_bms_power_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);

    TEST_ASSERT_EQUAL_INT(0, can_control_bms_power_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
}

/* Control bus, sender VCU: VCU_Status (0x090). */
static void test_vcu_status_packs_to_the_documented_layout_and_unpacks(void)
{
    struct can_control_vcu_status_t in;
    struct can_control_vcu_status_t out;
    uint8_t frame[8];
    /* byte 0 CRC; byte 1 counter and state; byte 2 Torque cut causes;
     * byte 3 causes of leaving Ready to Drive; byte 4 reported only;
     * byte 5 flags; bytes 6-7 torque, 123.4 Nm = 1234 = 0x04D2. */
    const uint8_t expected[8] = {0x11, 0x3E, 0xA0, 0x21, 0x30, 0x09, 0xD2, 0x04};

    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.crc = 0x11;
    in.counter = 14;
    in.state = CAN_CONTROL_VCU_STATUS_STATE_READY_TO_DRIVE_CHOICE;
    in.bms_power_timeout = 1;       /* byte 2 bit 5 */
    in.timing_fault = 1;            /* byte 2 bit 7 */
    in.bms_fault_reported = 1;      /* byte 3 bit 0 */
    in.startup_blocked = 1;         /* byte 3 bit 5 */
    in.limiter_active = 1;          /* byte 4 bit 4 */
    in.last_reset_not_power_on = 1; /* byte 4 bit 5 */
    in.brake_engaged = 1;           /* byte 5 bit 0 */
    in.enable_sent = 1;             /* byte 5 bit 3 */
    in.torque_sent = 1234;

    TEST_ASSERT_EQUAL_INT(8, can_control_vcu_status_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);

    TEST_ASSERT_EQUAL_INT(0, can_control_vcu_status_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_HEX32(0x090, CAN_CONTROL_VCU_STATUS_FRAME_ID);
}

/*
 * The inverter command, against the worked example in the CM Software Users
 * Manual, section 8.3, p.83-84: torque 30.0 Nm, speed 500 rpm, reverse,
 * enabled is "44 1 244 1 0 1 0 0" in decimal.
 */
static void test_inverter_command_matches_the_manual_example(void)
{
    struct can_control_inv_command_t in;
    uint8_t frame[8];
    const uint8_t expected[8] = {0x2C, 0x01, 0xF4, 0x01, 0x00, 0x01, 0x00, 0x00};

    memset(&in, 0, sizeof in);
    in.torque_command = 300;
    in.speed_command = 500;
    in.direction_command = 0;
    in.inverter_enable = 1;

    TEST_ASSERT_EQUAL_INT(8, can_control_inv_command_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);
}

/* The fields the manual example leaves at zero: Table 28 p.90, Table 35 p.100. */
static void test_inverter_command_carries_direction_counter_and_torque_limit(void)
{
    struct can_control_inv_command_t in;
    struct can_control_inv_command_t out;
    uint8_t frame[8];
    /* byte 4 direction forward; byte 5 enable (bit 0) and rolling counter 10
     * (bits 4-7); bytes 6-7 limit 240.0 Nm = 2400 = 0x0960. */
    const uint8_t expected[8] = {0x00, 0x00, 0x00, 0x00, 0x01, 0xA1, 0x60, 0x09};

    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.direction_command = 1;
    in.inverter_enable = 1;
    in.rolling_counter = 10;
    in.torque_limit = 2400;

    TEST_ASSERT_EQUAL_INT(8, can_control_inv_command_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);

    TEST_ASSERT_EQUAL_INT(0, can_control_inv_command_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
}

/* Control bus, sender inverter: Internal States (0x0AA), Table 64 p.145-148. */
static void test_inverter_internal_states_unpacks_only_the_signals_we_use(void)
{
    struct can_control_inv_internal_states_t out;
    uint8_t repacked[8];
    /* byte 0 VSM state 6 (motor running); byte 1 PWM frequency, byte 2
     * inverter state and the counter in byte 5 are not transcribed;
     * byte 6 enable state (bit 0) and enable lockout (bit 7);
     * byte 7 BMS active (bit 1) and BMS limiting motor torque (bit 2). */
    const uint8_t frame[8] = {0x06, 0x0C, 0x03, 0x00, 0x00, 0xA0, 0x81, 0x06};
    const uint8_t ours[8] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x06};

    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT(0,
        can_control_inv_internal_states_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_UINT8(
        CAN_CONTROL_INV_INTERNAL_STATES_VSM_STATE_MOTOR_RUNNING_CHOICE, out.vsm_state);
    TEST_ASSERT_EQUAL_UINT8(1, out.enable_state);
    TEST_ASSERT_EQUAL_UINT8(1, out.enable_lockout);
    TEST_ASSERT_EQUAL_UINT8(1, out.bms_active);
    TEST_ASSERT_EQUAL_UINT8(1, out.bms_limiting_motor_torque);

    TEST_ASSERT_EQUAL_INT(8,
        can_control_inv_internal_states_pack(repacked, &out, sizeof repacked));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ours, repacked, sizeof repacked);
}

/* Fault Codes (0x0AB), Table 65 p.149, with two faults from Tables 72-73. */
static void test_inverter_fault_codes_are_two_32_bit_words(void)
{
    struct can_control_inv_fault_codes_t out;
    /* POST "EEPROM Update Required" is 0x04000000 (Table 72); RUN "CAN
     * Command Message Lost" is 0x00000800 (Table 73). Lo word first, low
     * byte first. */
    const uint8_t frame[8] = {0x00, 0x00, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00};

    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT(0, can_control_inv_fault_codes_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_HEX32(0x04000000u, out.post_faults);
    TEST_ASSERT_EQUAL_HEX32(0x00000800u, out.run_faults);
}

/* Control bus, sender Energy Meter: Measurement (0x10D), EM manual p.10. */
static void test_energy_meter_measurement_is_two_little_endian_floats(void)
{
    struct can_control_em_measurement_t in;
    struct can_control_em_measurement_t out;
    uint8_t frame[8];
    /* IEEE-754: 1.0 is 0x3F800000, 400.0 is 0x43C80000. */
    const uint8_t expected[8] = {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0xC8, 0x43};

    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.current = 1.0f;
    in.voltage = 400.0f;

    TEST_ASSERT_EQUAL_INT(8, can_control_em_measurement_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);

    TEST_ASSERT_EQUAL_INT(0, can_control_em_measurement_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out.current);
    TEST_ASSERT_EQUAL_FLOAT(400.0f, out.voltage);
}

/* Control bus, sender iso175: PGN_Info_General, iso175 manual p.3 and p.11. */
static void test_imd_info_general_unpacks_resistance_and_alarm_bits(void)
{
    struct can_control_imd_info_general_t out;
    uint8_t repacked[8];
    /* Data 0-1 500 kOhm = 0x01F4; Data 2 status 0xFE (normal); Data 3
     * counter, not transcribed; Data 4-5 alarms: bit 4 iso alarm and bit 9
     * unsafe to start; Data 6 activity, not transcribed; Data 7 0xFF. */
    const uint8_t frame[8] = {0xF4, 0x01, 0xFE, 0x07, 0x10, 0x02, 0x01, 0xFF};
    const uint8_t ours[8] = {0xF4, 0x01, 0xFE, 0x00, 0x10, 0x02, 0x00, 0x00};

    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT(0, can_control_imd_info_general_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_UINT16(500, out.r_iso_corrected);
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_IMD_INFO_GENERAL_R_ISO_STATUS_NORMAL_CHOICE,
        out.r_iso_status);
    TEST_ASSERT_EQUAL_UINT8(1, out.iso_alarm);
    TEST_ASSERT_EQUAL_UINT8(1, out.unsafe_to_start);

    TEST_ASSERT_EQUAL_INT(8, can_control_imd_info_general_pack(repacked, &out, sizeof repacked));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ours, repacked, sizeof repacked);
    TEST_ASSERT_EQUAL_HEX32(0x18FF01F4u, CAN_CONTROL_IMD_INFO_GENERAL_FRAME_ID);
    TEST_ASSERT_EQUAL_INT(1, CAN_CONTROL_IMD_INFO_GENERAL_IS_EXTENDED);
}

/*
 * Control bus, sender charger: status (0x18FF50E5). The values are the
 * examples in the Elcon CAN Specification, p.2: Vout = 3201 is 320.1 V,
 * Iout = 582 is 58.2 A, high byte first.
 */
static void test_charger_status_is_big_endian_with_flags_in_byte_5(void)
{
    struct can_control_chg_status_t in;
    struct can_control_chg_status_t out;
    uint8_t frame[8];
    /* 3201 = 0x0C81, 582 = 0x0246, flags bit 4 = communication time-out. */
    const uint8_t expected[8] = {0x0C, 0x81, 0x02, 0x46, 0x10, 0x00, 0x00, 0x00};

    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.output_voltage = 3201;
    in.output_current = 582;
    in.comm_timeout = 1;

    TEST_ASSERT_EQUAL_INT(8, can_control_chg_status_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);

    TEST_ASSERT_EQUAL_INT(0, can_control_chg_status_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_HEX32(0x18FF50E5u, CAN_CONTROL_CHG_STATUS_FRAME_ID);
    TEST_ASSERT_EQUAL_INT(1, CAN_CONTROL_CHG_STATUS_IS_EXTENDED);
}

/* The request the BMS sends the charger (0x1806E5F4), same examples, p.2. */
static void test_charger_request_is_big_endian_with_control_in_byte_5(void)
{
    struct can_control_chg_request_t in;
    uint8_t frame[8];
    const uint8_t expected[8] = {0x0C, 0x81, 0x02, 0x46, 0x01, 0x00, 0x00, 0x00};

    memset(&in, 0, sizeof in);
    in.max_charge_voltage = 3201;
    in.max_charge_current = 582;
    in.control = CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE;

    TEST_ASSERT_EQUAL_INT(8, can_control_chg_request_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);
    TEST_ASSERT_EQUAL_HEX32(0x1806E5F4u, CAN_CONTROL_CHG_REQUEST_FRAME_ID);
    TEST_ASSERT_EQUAL_INT(1, CAN_CONTROL_CHG_REQUEST_IS_EXTENDED);
}

/* DAQ bus, sender BMS: one module's temperatures and bleed flags, 20 bytes. */
static void test_daq_cell_temps_is_a_20_byte_frame_with_bleed_flags(void)
{
    struct can_daq_bms_cell_temps_00_t in;
    struct can_daq_bms_cell_temps_00_t out;
    uint8_t frame[20];
    /* bytes 0-1 sensor 0 at 25.1 C = 251 = 0x00FB; bytes 14-15 sensor 7 at
     * -1.5 C = -15 = 0xFFF1; byte 16 bleed flags for cells 1 and 6. */
    const uint8_t expected[20] = {
        0xFB, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xF1, 0xFF,
        0x42, 0x00, 0x00, 0x00};

    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.temp0 = 251;
    in.temp7 = -15;
    in.bleed1 = 1;
    in.bleed6 = 1;

    TEST_ASSERT_EQUAL_INT(20, can_daq_bms_cell_temps_00_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);

    TEST_ASSERT_EQUAL_INT(0, can_daq_bms_cell_temps_00_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_HEX32(0x622, CAN_DAQ_BMS_CELL_TEMPS_00_FRAME_ID);
}

/* DAQ bus, sender VCU: the status bits. */
static void test_daq_status_packs_to_the_documented_layout_and_unpacks(void)
{
    struct can_daq_vcu_daq_status_t in;
    struct can_daq_vcu_daq_status_t out;
    uint8_t frame[8];
    /* byte 0: overrun seen (bit 0), Control bus state 2 (bits 2-3), card
     * state 1 (bits 4-7); byte 1 bit 0 events dropped; bytes 4-7 Records
     * dropped. */
    const uint8_t expected[8] = {0x19, 0x01, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01};

    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.overrun_seen = 1;
    in.control_bus_state = CAN_DAQ_VCU_DAQ_STATUS_CONTROL_BUS_STATE_BUS_OFF_CHOICE;
    in.card_state = CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_LOGGING_CHOICE;
    in.events_dropped = 1;
    in.records_dropped = 0x01020304u;

    TEST_ASSERT_EQUAL_INT(8, can_daq_vcu_daq_status_pack(frame, &in, sizeof frame));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);

    TEST_ASSERT_EQUAL_INT(0, can_daq_vcu_daq_status_unpack(&out, frame, sizeof frame));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_HEX32(0x290, CAN_DAQ_VCU_DAQ_STATUS_FRAME_ID);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bms_status_packs_to_the_documented_layout_and_unpacks);
    RUN_TEST(test_bms_power_carries_current_and_voltage_in_tenths);
    RUN_TEST(test_vcu_status_packs_to_the_documented_layout_and_unpacks);
    RUN_TEST(test_inverter_command_matches_the_manual_example);
    RUN_TEST(test_inverter_command_carries_direction_counter_and_torque_limit);
    RUN_TEST(test_inverter_internal_states_unpacks_only_the_signals_we_use);
    RUN_TEST(test_inverter_fault_codes_are_two_32_bit_words);
    RUN_TEST(test_energy_meter_measurement_is_two_little_endian_floats);
    RUN_TEST(test_imd_info_general_unpacks_resistance_and_alarm_bits);
    RUN_TEST(test_charger_status_is_big_endian_with_flags_in_byte_5);
    RUN_TEST(test_charger_request_is_big_endian_with_control_in_byte_5);
    RUN_TEST(test_daq_cell_temps_is_a_20_byte_frame_with_bleed_flags);
    RUN_TEST(test_daq_status_packs_to_the_documented_layout_and_unpacks);
    return UNITY_END();
}
