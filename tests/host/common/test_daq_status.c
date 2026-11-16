/*
 * Host tests for the DAQ bus status mapping
 * (App/common/logic/Inc/daq_status.h).
 *
 * can/daq.dbc, ControlBusState in VCU_DaqStatus and BMS_DaqStatus:
 * 0 ERROR_ACTIVE, 1 ERROR_PASSIVE, 2 BUS_OFF, 3 BUS_OFF_LATCHED.
 */
#include "daq_status.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_a_healthy_control_bus_is_error_active(void)
{
    const daq_status_t in = {0};

    TEST_ASSERT_EQUAL_UINT8(0u, daq_status_control_bus_state(&in));
}

static void test_the_control_bus_state_names_the_worst_of_its_flags(void)
{
    daq_status_t in = {0};

    in.control_bus_error_passive = true;
    TEST_ASSERT_EQUAL_UINT8(1u, daq_status_control_bus_state(&in));

    /* Bus-off outranks error-passive. */
    in.control_bus_off = true;
    TEST_ASSERT_EQUAL_UINT8(2u, daq_status_control_bus_state(&in));

    /* Latched outranks both, and stays reported between its once-per-second
     * retries, while the controller is running again. */
    in.control_bus_latched = true;
    TEST_ASSERT_EQUAL_UINT8(3u, daq_status_control_bus_state(&in));
    in.control_bus_off = false;
    in.control_bus_error_passive = false;
    TEST_ASSERT_EQUAL_UINT8(3u, daq_status_control_bus_state(&in));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_healthy_control_bus_is_error_active);
    RUN_TEST(test_the_control_bus_state_names_the_worst_of_its_flags);
    return UNITY_END();
}
