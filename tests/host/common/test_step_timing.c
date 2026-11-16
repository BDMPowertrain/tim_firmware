/*
 * Host tests for the Control step's timing (App/common/logic/Inc/step_timing.h,
 * ADR 0010): when the next step starts, what an Overrun is, and the worst
 * case kept since power-on.
 *
 * Spec (issue 36, "Control step, tasks and interrupts"): "An overrun is
 * counted and the next step starts on the next period boundary with no
 * catch-up." GLOSSARY.md, Overrun: "A Control step that had not finished
 * when its next period began."
 */
#include "step_timing.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- scheduling ---- */

static void test_a_step_that_finishes_in_time_waits_for_the_next_boundary(void)
{
    step_sched_t sched;
    step_sched_init(&sched, 10u, 1000u);

    TEST_ASSERT_EQUAL_UINT32(1010u, step_sched_next(&sched, 1003u));
    TEST_ASSERT_EQUAL_UINT32(1020u, step_sched_next(&sched, 1019u));
    TEST_ASSERT_EQUAL_UINT32(0u, sched.overruns);
}

static void test_a_step_still_running_at_its_next_boundary_is_one_overrun(void)
{
    step_sched_t sched;
    step_sched_init(&sched, 10u, 1000u);

    /* The boundary at 1010 began while the step ran: that boundary is lost
     * and the next step starts at 1020, not at once. */
    TEST_ASSERT_EQUAL_UINT32(1020u, step_sched_next(&sched, 1010u));
    TEST_ASSERT_EQUAL_UINT32(1u, sched.overruns);
    TEST_ASSERT_TRUE(sched.last_overran);
}

static void test_a_long_overrun_skips_every_lost_boundary_without_catch_up(void)
{
    step_sched_t sched;
    step_sched_init(&sched, 20u, 0u);

    /* Started at 0, finished at 67: boundaries 20, 40 and 60 are gone. A
     * step is one Overrun however many boundaries it covers. */
    TEST_ASSERT_EQUAL_UINT32(80u, step_sched_next(&sched, 67u));
    TEST_ASSERT_EQUAL_UINT32(1u, sched.overruns);

    /* And the step after it, on time, is not an Overrun. */
    TEST_ASSERT_EQUAL_UINT32(100u, step_sched_next(&sched, 85u));
    TEST_ASSERT_EQUAL_UINT32(1u, sched.overruns);
    TEST_ASSERT_FALSE(sched.last_overran);
}

static void test_scheduling_is_unaffected_by_the_millisecond_count_wrapping(void)
{
    step_sched_t sched;
    step_sched_init(&sched, 10u, 0xFFFFFFFAu);

    TEST_ASSERT_EQUAL_UINT32(4u, step_sched_next(&sched, 0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(0u, sched.overruns);
    TEST_ASSERT_EQUAL_UINT32(24u, step_sched_next(&sched, 15u));
    TEST_ASSERT_EQUAL_UINT32(1u, sched.overruns);
}

/* ---- statistics ---- */

static void test_stats_keep_the_last_step_and_the_worst_since_power_on(void)
{
    step_stats_t stats = {0};
    const uint32_t first[3] = {40u, 300u, 20u};
    const uint32_t second[3] = {90u, 100u, 15u};

    step_stats_record(&stats, 400u, first, 3u);
    step_stats_record(&stats, 250u, second, 3u);

    TEST_ASSERT_EQUAL_UINT32(250u, stats.last_step_us);
    TEST_ASSERT_EQUAL_UINT32(400u, stats.worst_step_us);
    TEST_ASSERT_EQUAL_UINT32(90u, stats.last_phase_us[0]);
    TEST_ASSERT_EQUAL_UINT32(100u, stats.last_phase_us[1]);
    TEST_ASSERT_EQUAL_UINT32(15u, stats.last_phase_us[2]);

    /* Each phase keeps its own worst, which need not come from the worst
     * step. */
    TEST_ASSERT_EQUAL_UINT32(90u, stats.worst_phase_us[0]);
    TEST_ASSERT_EQUAL_UINT32(300u, stats.worst_phase_us[1]);
    TEST_ASSERT_EQUAL_UINT32(20u, stats.worst_phase_us[2]);
}

static void test_stats_ignore_phases_beyond_the_maximum(void)
{
    step_stats_t stats = {0};
    uint32_t phases[STEP_PHASE_MAX + 1u];
    for (size_t i = 0; i <= STEP_PHASE_MAX; i++) {
        phases[i] = 7u;
    }

    step_stats_record(&stats, 50u, phases, STEP_PHASE_MAX + 1u);

    TEST_ASSERT_EQUAL_UINT32(7u, stats.worst_phase_us[STEP_PHASE_MAX - 1u]);
    TEST_ASSERT_EQUAL_UINT32(50u, stats.worst_step_us);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_step_that_finishes_in_time_waits_for_the_next_boundary);
    RUN_TEST(test_a_step_still_running_at_its_next_boundary_is_one_overrun);
    RUN_TEST(test_a_long_overrun_skips_every_lost_boundary_without_catch_up);
    RUN_TEST(test_scheduling_is_unaffected_by_the_millisecond_count_wrapping);
    RUN_TEST(test_stats_keep_the_last_step_and_the_worst_since_power_on);
    RUN_TEST(test_stats_ignore_phases_beyond_the_maximum);
    return UNITY_END();
}
