/*
 * Host tests for the Overrun watch (App/common/logic/Inc/overrun_watch.h,
 * ADR 0013).
 *
 * Issue 48: "Three overruns within one second produce a timing-fault signal;
 * the gate and the three-in-a-second rule are logic and host-tested across
 * the time wrap." "Each overrun queues one Event's worth of data: step
 * duration, slowest phase and its duration."
 */
#include "overrun_watch.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- three in a second ---- */

static void test_three_overruns_within_a_second_are_a_timing_fault(void)
{
    overrun_watch_t w = {0};

    TEST_ASSERT_FALSE(overrun_watch_record(&w, 5000u));
    TEST_ASSERT_FALSE(overrun_watch_record(&w, 5400u));
    TEST_ASSERT_TRUE(overrun_watch_record(&w, 5999u));
}

static void test_three_overruns_spanning_a_full_second_are_no_fault(void)
{
    overrun_watch_t w = {0};

    (void)overrun_watch_record(&w, 5000u);
    (void)overrun_watch_record(&w, 5400u);
    TEST_ASSERT_FALSE(overrun_watch_record(&w, 6000u));
}

static void test_the_first_overruns_after_power_on_are_judged_on_their_own(void)
{
    overrun_watch_t w = {0};

    /* The zeroed state holds no Overruns at time 0, so two real ones near
       time 0 are not three. */
    TEST_ASSERT_FALSE(overrun_watch_record(&w, 10u));
    TEST_ASSERT_FALSE(overrun_watch_record(&w, 20u));
    TEST_ASSERT_TRUE(overrun_watch_record(&w, 30u));
}

static void test_overruns_every_half_second_are_never_a_fault(void)
{
    overrun_watch_t w = {0};

    /* Any three span exactly one second, which is not within it. */
    for (uint32_t t = 0u; t < 600000u; t += 500u) {
        TEST_ASSERT_FALSE(overrun_watch_record(&w, t));
    }
}

static void test_each_overrun_with_two_others_in_the_second_before_it_is_a_fault(void)
{
    overrun_watch_t w = {0};

    (void)overrun_watch_record(&w, 1000u);
    (void)overrun_watch_record(&w, 1100u);
    TEST_ASSERT_TRUE(overrun_watch_record(&w, 1200u));
    TEST_ASSERT_TRUE(overrun_watch_record(&w, 1300u));
    /* 1200, 1300 and 2199: still three within a second. */
    TEST_ASSERT_TRUE(overrun_watch_record(&w, 2199u));
    /* 1300, 2199 and 2300: not. */
    TEST_ASSERT_FALSE(overrun_watch_record(&w, 2300u));
}

static void test_the_rule_holds_across_the_millisecond_wrap(void)
{
    overrun_watch_t w = {0};
    const uint32_t start = 0u - 300u;

    TEST_ASSERT_FALSE(overrun_watch_record(&w, start));
    TEST_ASSERT_FALSE(overrun_watch_record(&w, start + 290u));
    TEST_ASSERT_TRUE(overrun_watch_record(&w, start + 999u));

    overrun_watch_t v = {0};
    (void)overrun_watch_record(&v, start);
    (void)overrun_watch_record(&v, start + 500u);
    TEST_ASSERT_FALSE(overrun_watch_record(&v, start + 1000u));
}

/* ---- the Event's data ---- */

static void test_the_event_names_the_slowest_phase_and_its_time(void)
{
    const uint32_t phases[3] = {900u, 11500u, 40u};

    const overrun_event_t e = overrun_event_make(12440u, phases, 3u, false);

    TEST_ASSERT_EQUAL_UINT32(12440u, e.step_us);
    TEST_ASSERT_EQUAL_UINT8(1u, e.slowest_phase);
    TEST_ASSERT_EQUAL_UINT32(11500u, e.slowest_phase_us);
}

static void test_the_first_of_equal_phases_is_the_slowest(void)
{
    const uint32_t phases[4] = {10u, 700u, 700u, 5u};

    const overrun_event_t e = overrun_event_make(1415u, phases, 4u, false);

    TEST_ASSERT_EQUAL_UINT8(1u, e.slowest_phase);
    TEST_ASSERT_EQUAL_UINT32(700u, e.slowest_phase_us);
}

static void test_the_last_phase_can_be_the_slowest(void)
{
    const uint32_t phases[2] = {3u, 25000u};

    const overrun_event_t e = overrun_event_make(25003u, phases, 2u, false);

    TEST_ASSERT_EQUAL_UINT8(1u, e.slowest_phase);
    TEST_ASSERT_EQUAL_UINT32(25000u, e.slowest_phase_us);
    TEST_ASSERT_FALSE(e.extra_work);
}

/* Spec #36: "An overrun Event carries ... whether the open-wire check
   ran" (issue 64). */
static void test_the_event_says_whether_the_steps_extra_work_ran(void)
{
    const uint32_t phases[2] = {7600u, 12600u};

    TEST_ASSERT_TRUE(overrun_event_make(20200u, phases, 2u, true).extra_work);
    TEST_ASSERT_FALSE(overrun_event_make(20200u, phases, 2u, false).extra_work);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_three_overruns_within_a_second_are_a_timing_fault);
    RUN_TEST(test_three_overruns_spanning_a_full_second_are_no_fault);
    RUN_TEST(test_the_first_overruns_after_power_on_are_judged_on_their_own);
    RUN_TEST(test_overruns_every_half_second_are_never_a_fault);
    RUN_TEST(test_each_overrun_with_two_others_in_the_second_before_it_is_a_fault);
    RUN_TEST(test_the_rule_holds_across_the_millisecond_wrap);
    RUN_TEST(test_the_event_names_the_slowest_phase_and_its_time);
    RUN_TEST(test_the_first_of_equal_phases_is_the_slowest);
    RUN_TEST(test_the_last_phase_can_be_the_slowest);
    RUN_TEST(test_the_event_says_whether_the_steps_extra_work_ran);
    return UNITY_END();
}
