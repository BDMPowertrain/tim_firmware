/*
 * Host tests for each task's CPU share (App/common/logic/Inc/cpu_share.h).
 *
 * Spec (issue 47): "Per-task CPU share and stack high-water marks are
 * available to the shell." Housekeeping takes the run-time counters once a
 * second; the share is each task's part of the time counted since the last
 * time, in tenths of a percent.
 */
#include "cpu_share.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_the_shares_follow_the_run_time_in_the_window(void)
{
    const cpu_sample_t before[] = { { 1u, 1000u }, { 2u, 5000u }, { 3u, 0u } };
    const cpu_sample_t now[]    = { { 1u, 1750u }, { 2u, 5200u }, { 3u, 50u } };
    uint32_t share[3];

    /* Rises of 750, 200 and 50 in 1000. */
    cpu_share_compute(before, 3u, now, 3u, share);
    TEST_ASSERT_EQUAL_UINT32(750u, share[0]);
    TEST_ASSERT_EQUAL_UINT32(200u, share[1]);
    TEST_ASSERT_EQUAL_UINT32(50u, share[2]);
}

static void test_tasks_are_matched_by_number_not_by_position(void)
{
    const cpu_sample_t before[] = { { 7u, 100u }, { 4u, 900u } };
    const cpu_sample_t now[]    = { { 4u, 1000u }, { 7u, 400u } };
    uint32_t share[2];

    /* Task 4 rose 100, task 7 rose 300. */
    cpu_share_compute(before, 2u, now, 2u, share);
    TEST_ASSERT_EQUAL_UINT32(250u, share[0]);
    TEST_ASSERT_EQUAL_UINT32(750u, share[1]);
}

static void test_a_counter_that_wrapped_still_counts_its_rise(void)
{
    const cpu_sample_t before[] = { { 1u, 0xFFFFFF00u }, { 2u, 0u } };
    const cpu_sample_t now[]    = { { 1u, 0x00000200u }, { 2u, 0x300u } };
    uint32_t share[2];

    /* Task 1 rose 0x300 across the wrap, task 2 rose 0x300. */
    cpu_share_compute(before, 2u, now, 2u, share);
    TEST_ASSERT_EQUAL_UINT32(500u, share[0]);
    TEST_ASSERT_EQUAL_UINT32(500u, share[1]);
}

static void test_a_new_task_counts_from_its_creation(void)
{
    const cpu_sample_t before[] = { { 1u, 1000u } };
    const cpu_sample_t now[]    = { { 1u, 1900u }, { 9u, 100u } };
    uint32_t share[2];

    cpu_share_compute(before, 1u, now, 2u, share);
    TEST_ASSERT_EQUAL_UINT32(900u, share[0]);
    TEST_ASSERT_EQUAL_UINT32(100u, share[1]);
}

static void test_the_first_gathering_counts_from_power_on(void)
{
    const cpu_sample_t now[] = { { 1u, 300u }, { 2u, 700u } };
    uint32_t share[2];

    cpu_share_compute(NULL, 0u, now, 2u, share);
    TEST_ASSERT_EQUAL_UINT32(300u, share[0]);
    TEST_ASSERT_EQUAL_UINT32(700u, share[1]);
}

static void test_shares_round_to_the_nearest_tenth_of_a_percent(void)
{
    const cpu_sample_t now[] = { { 1u, 1u }, { 2u, 2u } };
    uint32_t share[2];

    /* 1/3 and 2/3: 333.3 and 666.7. */
    cpu_share_compute(NULL, 0u, now, 2u, share);
    TEST_ASSERT_EQUAL_UINT32(333u, share[0]);
    TEST_ASSERT_EQUAL_UINT32(667u, share[1]);
}

static void test_a_window_with_no_time_gives_every_task_zero(void)
{
    const cpu_sample_t before[] = { { 1u, 50u }, { 2u, 60u } };
    uint32_t share[2] = { 99u, 99u };

    cpu_share_compute(before, 2u, before, 2u, share);
    TEST_ASSERT_EQUAL_UINT32(0u, share[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, share[1]);
}

static void test_a_full_second_of_cycles_does_not_overflow(void)
{
    /* 170 MHz for one second, nearly all of it idle. */
    const cpu_sample_t before[] = { { 1u, 0u }, { 2u, 0u } };
    const cpu_sample_t now[]    = { { 1u, 169830000u }, { 2u, 170000u } };
    uint32_t share[2];

    cpu_share_compute(before, 2u, now, 2u, share);
    TEST_ASSERT_EQUAL_UINT32(999u, share[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, share[1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_shares_follow_the_run_time_in_the_window);
    RUN_TEST(test_tasks_are_matched_by_number_not_by_position);
    RUN_TEST(test_a_counter_that_wrapped_still_counts_its_rise);
    RUN_TEST(test_a_new_task_counts_from_its_creation);
    RUN_TEST(test_the_first_gathering_counts_from_power_on);
    RUN_TEST(test_shares_round_to_the_nearest_tenth_of_a_percent);
    RUN_TEST(test_a_window_with_no_time_gives_every_task_zero);
    RUN_TEST(test_a_full_second_of_cycles_does_not_overflow);
    return UNITY_END();
}
