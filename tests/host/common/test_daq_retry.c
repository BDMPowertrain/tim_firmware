/*
 * Host tests for the DAQ bus bus-off retry (App/common/logic/Inc/daq_retry.h).
 *
 * Spec (issue 46): "a CAN2 bus-off is retried once per second forever and
 * affects nothing else". Issue 36: "DAQ bus isolation: ... a bus-off retry
 * once per second forever".
 */
#include "daq_retry.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_a_running_bus_is_never_restarted(void)
{
    daq_retry_t r = {0};

    for (uint32_t t = 0u; t < 10000u; t += 100u) {
        TEST_ASSERT_FALSE(daq_retry_update(&r, false, t));
    }
    TEST_ASSERT_EQUAL_UINT32(0u, r.bus_offs);
    TEST_ASSERT_EQUAL_UINT32(0u, r.restarts);
}

static void test_a_bus_off_is_restarted_one_second_after_it_was_seen(void)
{
    daq_retry_t r = {0};

    TEST_ASSERT_FALSE(daq_retry_update(&r, true, 5000u));
    TEST_ASSERT_EQUAL_UINT32(1u, r.bus_offs);
    TEST_ASSERT_FALSE(daq_retry_update(&r, true, 5100u));
    TEST_ASSERT_FALSE(daq_retry_update(&r, true, 5999u));
    TEST_ASSERT_TRUE(daq_retry_update(&r, true, 6000u));
    TEST_ASSERT_EQUAL_UINT32(1u, r.restarts);
}

static void test_a_bus_that_stays_off_is_retried_every_second_forever(void)
{
    daq_retry_t r = {0};
    uint32_t restarts = 0u;

    /* Polled every 100 ms for an hour with the controller never rejoining. */
    for (uint32_t t = 0u; t <= 3600000u; t += 100u) {
        if (daq_retry_update(&r, true, t)) {
            TEST_ASSERT_EQUAL_UINT32(0u, t % 1000u);
            restarts++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3600u, restarts);
    TEST_ASSERT_EQUAL_UINT32(3600u, r.restarts);
    TEST_ASSERT_EQUAL_UINT32(1u, r.bus_offs);
}

static void test_a_new_bus_off_after_rejoining_waits_its_own_full_second(void)
{
    daq_retry_t r = {0};

    (void)daq_retry_update(&r, true, 0u);
    TEST_ASSERT_TRUE(daq_retry_update(&r, true, 1000u));
    TEST_ASSERT_FALSE(daq_retry_update(&r, false, 1100u)); /* rejoined */

    TEST_ASSERT_FALSE(daq_retry_update(&r, true, 1500u));  /* off again */
    TEST_ASSERT_EQUAL_UINT32(2u, r.bus_offs);
    TEST_ASSERT_FALSE(daq_retry_update(&r, true, 2000u));
    TEST_ASSERT_TRUE(daq_retry_update(&r, true, 2500u));
}

static void test_the_retry_survives_the_millisecond_count_wrapping(void)
{
    daq_retry_t r = {0};

    (void)daq_retry_update(&r, true, 0xFFFFFF00u);
    TEST_ASSERT_FALSE(daq_retry_update(&r, true, 0x00000000u)); /* 256 ms on */
    TEST_ASSERT_FALSE(daq_retry_update(&r, true, 0x000002E7u)); /* 999 ms on */
    TEST_ASSERT_TRUE(daq_retry_update(&r, true, 0x000002E8u));  /* 1000 ms on */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_running_bus_is_never_restarted);
    RUN_TEST(test_a_bus_off_is_restarted_one_second_after_it_was_seen);
    RUN_TEST(test_a_bus_that_stays_off_is_retried_every_second_forever);
    RUN_TEST(test_a_new_bus_off_after_rejoining_waits_its_own_full_second);
    RUN_TEST(test_the_retry_survives_the_millisecond_count_wrapping);
    return UNITY_END();
}
