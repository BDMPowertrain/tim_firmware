/*
 * Host tests for the Control step's watch on Housekeeping
 * (App/common/logic/Inc/stall_watch.h).
 *
 * Spec (issue 47): "Housekeeping bumps a counter; the control step sets a
 * 'background stalled' warning if it stops advancing for 1 s, and resets
 * nothing." can/daq.dbc, BackgroundStalled: "Housekeeping has not run for
 * 1 s."
 *
 * The VCU step calls it every 10 ms, the BMS step every 20 ms.
 */
#include "stall_watch.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_a_counter_bumped_every_100_ms_is_never_stalled(void)
{
    stall_watch_t w = {0};
    uint32_t count = 0u;

    for (uint32_t t = 0u; t < 60000u; t += 10u) {
        if (t % 100u == 0u) {
            count++;
        }
        TEST_ASSERT_FALSE(stall_watch_update(&w, count, t));
    }
}

static void test_a_counter_that_stops_is_stalled_after_1_s(void)
{
    stall_watch_t w = {0};

    TEST_ASSERT_FALSE(stall_watch_update(&w, 5u, 2000u));
    TEST_ASSERT_FALSE(stall_watch_update(&w, 5u, 2500u));
    TEST_ASSERT_FALSE(stall_watch_update(&w, 5u, 2990u));
    TEST_ASSERT_TRUE(stall_watch_update(&w, 5u, 3000u));
    TEST_ASSERT_TRUE(stall_watch_update(&w, 5u, 60000u));
}

static void test_the_warning_clears_when_the_counter_moves_again(void)
{
    stall_watch_t w = {0};

    (void)stall_watch_update(&w, 5u, 0u);
    TEST_ASSERT_TRUE(stall_watch_update(&w, 5u, 1500u));
    TEST_ASSERT_FALSE(stall_watch_update(&w, 6u, 1510u));
    TEST_ASSERT_FALSE(stall_watch_update(&w, 6u, 2500u));
    TEST_ASSERT_TRUE(stall_watch_update(&w, 6u, 2510u));
}

static void test_the_first_second_counts_from_the_first_call(void)
{
    stall_watch_t w = {0};

    /* Housekeeping has not run yet when the first step does: its counter
       reads 0, as the zeroed state does, and that is no warning until a
       full second has passed from the first step. */
    TEST_ASSERT_FALSE(stall_watch_update(&w, 0u, 50000u));
    TEST_ASSERT_FALSE(stall_watch_update(&w, 0u, 50990u));
    TEST_ASSERT_TRUE(stall_watch_update(&w, 0u, 51000u));
}

static void test_housekeeping_that_never_starts_is_stalled_after_1_s(void)
{
    stall_watch_t w = {0};

    for (uint32_t t = 10u; t < 1010u; t += 10u) {
        TEST_ASSERT_FALSE(stall_watch_update(&w, 0u, t));
    }
    TEST_ASSERT_TRUE(stall_watch_update(&w, 0u, 1010u));
}

static void test_the_millisecond_count_wrapping_changes_nothing(void)
{
    stall_watch_t w = {0};
    const uint32_t start = 0u - 400u;

    TEST_ASSERT_FALSE(stall_watch_update(&w, 9u, start));
    TEST_ASSERT_FALSE(stall_watch_update(&w, 9u, start + 999u));
    TEST_ASSERT_TRUE(stall_watch_update(&w, 9u, start + 1000u));
}

static void test_the_counter_wrapping_is_still_movement(void)
{
    stall_watch_t w = {0};

    (void)stall_watch_update(&w, 0xFFFFFFFFu, 0u);
    TEST_ASSERT_FALSE(stall_watch_update(&w, 0u, 900u));
    TEST_ASSERT_FALSE(stall_watch_update(&w, 0u, 1800u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_counter_bumped_every_100_ms_is_never_stalled);
    RUN_TEST(test_a_counter_that_stops_is_stalled_after_1_s);
    RUN_TEST(test_the_warning_clears_when_the_counter_moves_again);
    RUN_TEST(test_the_first_second_counts_from_the_first_call);
    RUN_TEST(test_housekeeping_that_never_starts_is_stalled_after_1_s);
    RUN_TEST(test_the_millisecond_count_wrapping_changes_nothing);
    RUN_TEST(test_the_counter_wrapping_is_still_movement);
    return UNITY_END();
}
