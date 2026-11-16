/*
 * Host tests for the VCU's DAQ bus status frame
 * (App/vcu/logic/Inc/vcu_daq.h, App/common/logic/Inc/daq_status.h).
 *
 * Spec (issue 46): "Status bits on the DAQ bus carry: single overrun seen,
 * background stalled, Control bus state, and placeholders for Data log card
 * state and dropped count". can/daq.dbc, VCU_DaqStatus (0x290): OverrunSeen
 * bit 0, BackgroundStalled bit 1, ControlBusState bits 2-3 (0 ERROR_ACTIVE,
 * 1 ERROR_PASSIVE, 2 BUS_OFF, 3 BUS_OFF_LATCHED), CardState bits 4-7,
 * EventsDropped bit 8, AllocationFailed bit 9, RecordsDropped bytes 4-7.
 */
#include <string.h>

#include "can_daq.h"
#include "daq_status.h"
#include "unity.h"
#include "vcu_daq.h"

void setUp(void) {}
void tearDown(void) {}

static struct can_daq_vcu_daq_status_t build(const daq_status_t *in)
{
    uint8_t frame[CAN_DAQ_VCU_DAQ_STATUS_LENGTH];
    struct can_daq_vcu_daq_status_t out;

    memset(frame, 0xA5, sizeof(frame));
    vcu_daq_build_status(in, frame);
    TEST_ASSERT_EQUAL_INT(0, can_daq_vcu_daq_status_unpack(&out, frame, sizeof(frame)));
    return out;
}

static void test_a_healthy_image_sends_an_all_zero_frame(void)
{
    const daq_status_t in = {0};
    uint8_t frame[CAN_DAQ_VCU_DAQ_STATUS_LENGTH];
    const uint8_t zero[CAN_DAQ_VCU_DAQ_STATUS_LENGTH] = {0};

    memset(frame, 0xA5, sizeof(frame));
    vcu_daq_build_status(&in, frame);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, frame, sizeof(frame));
}

static void test_one_overrun_since_power_on_sets_overrun_seen(void)
{
    daq_status_t in = {0};

    in.overruns = 1u;
    TEST_ASSERT_EQUAL_UINT8(1u, build(&in).overrun_seen);
    in.overruns = 0xFFFFFFFFu;
    TEST_ASSERT_EQUAL_UINT8(1u, build(&in).overrun_seen);
}

static void test_background_stalled_is_carried(void)
{
    daq_status_t in = {0};

    in.background_stalled = true;
    const struct can_daq_vcu_daq_status_t out = build(&in);
    TEST_ASSERT_EQUAL_UINT8(1u, out.background_stalled);
    TEST_ASSERT_EQUAL_UINT8(0u, out.overrun_seen);
}

/* Which flag wins is daq_status's (tests/host/common/test_daq_status.c);
 * this checks the state reaches bits 2-3. */
static void test_the_control_bus_state_is_carried(void)
{
    daq_status_t in = {0};

    in.control_bus_latched = true;
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_VCU_DAQ_STATUS_CONTROL_BUS_STATE_BUS_OFF_LATCHED_CHOICE,
                            build(&in).control_bus_state);
}

static void test_the_data_log_placeholders_are_carried(void)
{
    daq_status_t in = {0};

    in.card_state = CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_ERROR_CHOICE;
    in.records_dropped = 0x12345678u;
    const struct can_daq_vcu_daq_status_t out = build(&in);
    TEST_ASSERT_EQUAL_UINT8(2u, out.card_state);
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, out.records_dropped);
    TEST_ASSERT_EQUAL_UINT8(0u, out.control_bus_state);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_healthy_image_sends_an_all_zero_frame);
    RUN_TEST(test_one_overrun_since_power_on_sets_overrun_seen);
    RUN_TEST(test_background_stalled_is_carried);
    RUN_TEST(test_the_control_bus_state_is_carried);
    RUN_TEST(test_the_data_log_placeholders_are_carried);
    return UNITY_END();
}
