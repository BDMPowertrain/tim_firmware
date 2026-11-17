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
    };
    const uint8_t want[EVENT_DATA_SIZE] = {
        0x39, 0x30, 0, 0, 0x00, 0x01, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
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

static void test_names(void)
{
    TEST_ASSERT_EQUAL_STRING("data log card", event_code_name(EVENT_DATA_LOG_CARD));
    TEST_ASSERT_EQUAL_STRING("VCU state", event_code_name(EVENT_VCU_STATE));
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
    RUN_TEST(test_the_data_log_card_event);
    RUN_TEST(test_names);
    return UNITY_END();
}
