/*
 * Host tests for the Event codes' data layouts
 * (App/common/logic/Inc/event_codes.h): the bytes are what a stored
 * Event holds, so they are checked against literals.
 */
#include <string.h>

#include "event_codes.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_the_boot_event_after_a_crash(void)
{
    const reset_report_t reset = {
        .cause = RESET_CAUSE_SOFTWARE,
        .layout_found = true,
        .non_power_on_resets = 0x01020304u,
        .breadcrumb = { .step_count = 9u, .phase = 1u, .overruns = 2u },
        .crash = { .cause = CRASH_ASSERT, .task = "shell", .address = 0x08001234u },
    };
    const event_boot_t boot = {
        .image = PARAMS_IMAGE_BMS,
        .build_id = 0xA1B2C3D4u,
        .dirty = true,
        .reset = &reset,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        PARAMS_IMAGE_BMS, RESET_CAUSE_SOFTWARE,
        EVENT_BOOT_DIRTY | EVENT_BOOT_CRASH_NEXT | EVENT_BOOT_CRUMB_NEXT, 0,
        0xD4, 0xC3, 0xB2, 0xA1, 0x04, 0x03, 0x02, 0x01, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_boot(data, &boot);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

static void test_a_power_on_boot_has_nothing_following(void)
{
    const reset_report_t reset = {
        .cause = RESET_CAUSE_POWER_ON,
        .layout_found = true,
        .breadcrumb = { .phase = BREADCRUMB_PHASE_NONE },
    };
    const event_boot_t boot = { .image = PARAMS_IMAGE_VCU, .reset = &reset };
    uint8_t data[EVENT_DATA_SIZE];

    event_data_boot(data, &boot);
    TEST_ASSERT_EQUAL_HEX8(0u, data[2]);
}

static void test_the_overrun_event(void)
{
    const overrun_event_t overrun = {
        .step_us = 12345u, .slowest_phase_us = 0x0100u, .slowest_phase = 3u,
        .extra_work = true,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        0x39, 0x30, 0, 0, 0x00, 0x01, 0, 0, 3, 1, 0, 0, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_overrun(data, &overrun);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

static void test_the_crash_event(void)
{
    const crash_record_t crash = { .cause = CRASH_HARD_FAULT, .task = "controlS",
                                   .address = 0x08004000u };
    const uint8_t want[EVENT_DATA_SIZE] = {
        CRASH_HARD_FAULT, 0, 0, 0, 0x00, 0x40, 0x00, 0x08,
        'c', 'o', 'n', 't', 'r', 'o', 'l', 'S',
    };
    uint8_t data[EVENT_DATA_SIZE];

    event_data_crash(data, &crash);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

/* A VCU state transition: from, to, cause, the two voltages read, every
   cause that applied. */
static void test_the_vcu_state_event(void)
{
    const event_vcu_state_t state = {
        .from = 3u, .to = 2u, .cause = 5u,
        .bus_dv = -2, .pack_dv = 4000u, .causes = 0x0013u,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        3, 2, 5, 0, 0xFE, 0xFF, 0xA0, 0x0F, 0x13, 0x00, 0, 0, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_vcu_state(data, &state);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

/* A Torque cut starting or ending (issue 57): cause, started, state,
   both Pedal travels, the four sensor statuses. */
static void test_the_torque_cut_event(void)
{
    const event_torque_cut_t cut = {
        .cause = 3u, .started = true, .state = 4u,
        .travel = { 0x0102u, 0x0304u },
        .apps_status = { 0u, 4u }, .brake_status = { 2u, 0u },
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        3, 1, 4, 0, 0x02, 0x01, 0x04, 0x03, 0, 4, 2, 0, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_torque_cut(data, &cut);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

/* An inverter-reported fault (issue 58): all 64 bits, POST then run. */
static void test_the_inverter_fault_event(void)
{
    const uint8_t want[EVENT_DATA_SIZE] = {
        0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_inverter_fault(data, 0x01020304u, 0x05060708u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

/* A fault-clear attempt or its result (issue 58): what happened, the state,
   the 64 fault bits then, the time since the 0x0C1 went. */
static void test_the_inverter_fault_clear_event(void)
{
    const event_fault_clear_t clear = {
        .result = EVENT_FAULT_CLEAR_FAILED, .state = 3u,
        .post_faults = 0x11223344u, .run_faults = 0x55667788u, .since_ms = 0x0400u,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        2, 3, 0, 0, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0x00, 0x04, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_fault_clear(data, &clear);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

/* Discharge slow (issue 58): the bus voltage, and how long the Shutdown
   Circuit input has been open. */
static void test_the_discharge_slow_event(void)
{
    const uint8_t want[EVENT_DATA_SIZE] = {
        0x58, 0x02, 0, 0, 0x88, 0x13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_discharge_slow(data, 600, 5000u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

/* The Energy Meter's Violation bit set (issue 58): the VCU state. */
static void test_the_energy_meter_violation_event(void)
{
    const uint8_t want[EVENT_DATA_SIZE] = { 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_em_violation(data, 4u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

/* A BMS fault raised (issue 63): the fault, the state after, where it was
   seen, which cell or sensor, its reading and the devices read. */
static void test_the_bms_fault_event(void)
{
    const event_bms_fault_t fault = {
        .fault = 4u, .state = 3u, .where = EVENT_BMS_SENSOR, .index = 0x0102u,
        .reading_mv = 0x0A0Bu, .devices_read = 0xFFF7u,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        4, 3, 2, 0, 0x02, 0x01, 0x0B, 0x0A, 0xF7, 0xFF, 0, 0, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_bms_fault(data, &fault);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
    TEST_ASSERT_EQUAL_STRING("BMS fault", event_code_name(EVENT_BMS_FAULT));
}

/* A Data log card failure (issue 53): the state it went to, the FatFS
   result, Records written and dropped so far. */
static void test_the_data_log_card_event(void)
{
    const event_data_log_card_t card = {
        .state = 3u, .result = 9u, .written = 0x01020304u, .dropped = 0x0A0Bu,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        3, 9, 0, 0, 0x04, 0x03, 0x02, 0x01, 0x0B, 0x0A, 0, 0, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_data_log_card(data, &card);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
}

/* Issue 67: a charge state change, with the highest and lowest cell and
   the Charge limit at the time. */
static void test_the_charge_event(void)
{
    const event_charge_t charge = {
        .from = 1u, .to = 2u, .cause = 0u,
        .cells = { .highest_mv = 0x1004u, .highest = 0x0080u, .lowest_mv = 0x0F05u, .lowest = 1u },
        .limit_da = 0x0102u,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        1, 2, 0, 0, 0x04, 0x10, 0x80, 0x00, 0x05, 0x0F, 0x01, 0x00, 0x02, 0x01, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_charge(data, &charge);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
    TEST_ASSERT_EQUAL_STRING("charge", event_code_name(EVENT_CHARGE));
}

/* Issue 67, ADR 0019: the rested cells after Charge complete. */
static void test_the_charge_rested_event(void)
{
    const event_charge_cells_t cells = {
        .highest_mv = 0x1004u, .highest = 0x0080u, .lowest_mv = 0x0F05u, .lowest = 1u,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        0x04, 0x10, 0x80, 0x00, 0x05, 0x0F, 0x01, 0x00, 0x60, 0xEA, 0x00, 0x00, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_charge_rested(data, &cells, 60000u);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
    TEST_ASSERT_EQUAL_STRING("charge rested cells", event_code_name(EVENT_CHARGE_RESTED));
}

/* Issue 67: the charger's status flags changed. */
static void test_the_charger_flags_event(void)
{
    const event_charger_flags_t f = {
        .from = 0x02u, .to = 0x1Bu, .charge_state = 3u,
        .output_dv = 0x1314u, .output_da = 0x0015u,
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        0x02, 0x1B, 3, 0, 0x14, 0x13, 0x15, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, 0xAA, sizeof data);
    event_data_charger_flags(data, &f);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, data, EVENT_DATA_SIZE);
    TEST_ASSERT_EQUAL_STRING("charger flags", event_code_name(EVENT_CHARGER_FLAGS));
}

static void test_names(void)
{
    TEST_ASSERT_EQUAL_STRING("data log card", event_code_name(EVENT_DATA_LOG_CARD));
    TEST_ASSERT_EQUAL_STRING("VCU state", event_code_name(EVENT_VCU_STATE));
    TEST_ASSERT_EQUAL_STRING("torque cut", event_code_name(EVENT_TORQUE_CUT));
    TEST_ASSERT_EQUAL_STRING("inverter fault", event_code_name(EVENT_INVERTER_FAULT));
    TEST_ASSERT_EQUAL_STRING("inverter fault clear", event_code_name(EVENT_INVERTER_FAULT_CLEAR));
    TEST_ASSERT_EQUAL_STRING("discharge slow", event_code_name(EVENT_DISCHARGE_SLOW));
    TEST_ASSERT_EQUAL_STRING("energy meter violation", event_code_name(EVENT_EM_VIOLATION));
    TEST_ASSERT_EQUAL_STRING("boot", event_code_name(EVENT_BOOT));
    TEST_ASSERT_EQUAL_STRING("bus-off", event_code_name(EVENT_BUS_OFF));
    TEST_ASSERT_EQUAL_STRING("?", event_code_name(0xBEEFu));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_boot_event_after_a_crash);
    RUN_TEST(test_a_power_on_boot_has_nothing_following);
    RUN_TEST(test_the_overrun_event);
    RUN_TEST(test_the_crash_event);
    RUN_TEST(test_the_vcu_state_event);
    RUN_TEST(test_the_torque_cut_event);
    RUN_TEST(test_the_inverter_fault_event);
    RUN_TEST(test_the_inverter_fault_clear_event);
    RUN_TEST(test_the_discharge_slow_event);
    RUN_TEST(test_the_energy_meter_violation_event);
    RUN_TEST(test_the_bms_fault_event);
    RUN_TEST(test_the_data_log_card_event);
    RUN_TEST(test_the_charge_event);
    RUN_TEST(test_the_charge_rested_event);
    RUN_TEST(test_the_charger_flags_event);
    RUN_TEST(test_names);
    return UNITY_END();
}
