/*
 * Host tests for the Kick gate (App/common/logic/Inc/kick_gate.h, ADR 0013).
 *
 * Issue 48: "The only kick after start-up is at the start of a control step
 * and is gated on the previous step having completed every phase; the first
 * step kicks unconditionally." ADR 0013: "made only if the previous step ran
 * every phase in order to the end."
 */
#include "kick_gate.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_the_first_step_kicks(void)
{
    kick_gate_t gate;
    kick_gate_init(&gate, 3u);

    TEST_ASSERT_TRUE(kick_gate_step_start(&gate));
}

static void run_phases(kick_gate_t *gate, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        kick_gate_phase_done(gate, i);
    }
}

static void test_a_step_after_one_that_ran_every_phase_kicks(void)
{
    kick_gate_t gate;
    kick_gate_init(&gate, 3u);

    for (int step = 0; step < 1000; step++) {
        TEST_ASSERT_TRUE(kick_gate_step_start(&gate));
        run_phases(&gate, 3u);
    }
}

static void test_a_step_after_one_that_stopped_short_does_not_kick(void)
{
    kick_gate_t gate;
    kick_gate_init(&gate, 3u);

    (void)kick_gate_step_start(&gate);
    run_phases(&gate, 2u);

    TEST_ASSERT_FALSE(kick_gate_step_start(&gate));
}

static void test_a_step_after_one_that_ran_no_phase_does_not_kick(void)
{
    kick_gate_t gate;
    kick_gate_init(&gate, 3u);

    (void)kick_gate_step_start(&gate);

    TEST_ASSERT_FALSE(kick_gate_step_start(&gate));
}

static void test_a_step_after_one_that_ran_its_phases_out_of_order_does_not_kick(void)
{
    kick_gate_t gate;
    kick_gate_init(&gate, 3u);

    (void)kick_gate_step_start(&gate);
    kick_gate_phase_done(&gate, 0u);
    kick_gate_phase_done(&gate, 2u);
    kick_gate_phase_done(&gate, 1u);

    TEST_ASSERT_FALSE(kick_gate_step_start(&gate));
}

static void test_a_step_after_one_that_ran_a_phase_twice_does_not_kick(void)
{
    kick_gate_t gate;
    kick_gate_init(&gate, 3u);

    /* Three calls, as a complete step makes, but phase 2 never ran. */
    (void)kick_gate_step_start(&gate);
    kick_gate_phase_done(&gate, 0u);
    kick_gate_phase_done(&gate, 1u);
    kick_gate_phase_done(&gate, 1u);

    TEST_ASSERT_FALSE(kick_gate_step_start(&gate));
}

static void test_a_step_after_one_that_ran_a_phase_too_many_does_not_kick(void)
{
    kick_gate_t gate;
    kick_gate_init(&gate, 3u);

    (void)kick_gate_step_start(&gate);
    run_phases(&gate, 3u);
    kick_gate_phase_done(&gate, 0u);

    TEST_ASSERT_FALSE(kick_gate_step_start(&gate));
}

static void test_a_complete_step_after_a_withheld_kick_kicks_again(void)
{
    kick_gate_t gate;
    kick_gate_init(&gate, 3u);

    (void)kick_gate_step_start(&gate);
    kick_gate_phase_done(&gate, 1u);
    kick_gate_phase_done(&gate, 0u);
    kick_gate_phase_done(&gate, 2u);
    TEST_ASSERT_FALSE(kick_gate_step_start(&gate));

    /* Only the step after a complete one Kicks: the gap is not made good,
       and an out-of-order step does not taint the one after it. */
    run_phases(&gate, 3u);
    TEST_ASSERT_TRUE(kick_gate_step_start(&gate));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_first_step_kicks);
    RUN_TEST(test_a_step_after_one_that_ran_every_phase_kicks);
    RUN_TEST(test_a_step_after_one_that_stopped_short_does_not_kick);
    RUN_TEST(test_a_step_after_one_that_ran_no_phase_does_not_kick);
    RUN_TEST(test_a_step_after_one_that_ran_its_phases_out_of_order_does_not_kick);
    RUN_TEST(test_a_step_after_one_that_ran_a_phase_twice_does_not_kick);
    RUN_TEST(test_a_step_after_one_that_ran_a_phase_too_many_does_not_kick);
    RUN_TEST(test_a_complete_step_after_a_withheld_kick_kicks_again);
    return UNITY_END();
}
