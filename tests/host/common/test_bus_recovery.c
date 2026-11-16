/*
 * Host tests for bus recovery (App/common/logic/Inc/bus_recovery.h,
 * ADR 0004, ADR 0005).
 *
 * Spec (issue 36): "Bus-off on the Control bus: every mailbox goes INVALID
 * at once; restart after 10 ms; on the third bus-off without 1 s clean in
 * between, one retry per second and a latched fault. [...] Error-passive is
 * a status bit and an Event only." Reaction table: "Bus-off latch (clears
 * after 5 s clean with all mailboxes VALID)".
 */
#include "bus_recovery.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* The bus under test: the interrupt's count and the controller's state. */
static bus_recovery_t br;
static bus_recovery_input_t in;

static void reset(void)
{
    br = (bus_recovery_t){0};
    in = (bus_recovery_input_t){ .latch_may_clear = true };
}

static bus_recovery_action_t step(uint32_t now_ms)
{
    return bus_recovery_update(&br, &in, now_ms);
}

/* ---- one bus-off ---- */

static void test_a_bus_off_is_reported_in_the_step_that_sees_it(void)
{
    reset();
    TEST_ASSERT_FALSE(step(1000u).bus_off);

    in.bus_off_count = 1u;
    const bus_recovery_action_t a = step(1010u);

    TEST_ASSERT_TRUE(a.bus_off);
    TEST_ASSERT_FALSE(a.restart);
    TEST_ASSERT_TRUE(br.off);
    TEST_ASSERT_EQUAL_UINT32(1u, br.counters.bus_offs);

    /* Reported once, not on every step it lasts. */
    TEST_ASSERT_FALSE(step(1015u).bus_off);
}

static void test_the_controller_is_restarted_10_ms_after_the_bus_off(void)
{
    reset();
    in.bus_off_count = 1u;
    (void)step(1000u);

    TEST_ASSERT_FALSE(step(1009u).restart);
    TEST_ASSERT_TRUE(step(1010u).restart);
    TEST_ASSERT_FALSE(br.off);
    TEST_ASSERT_EQUAL_UINT32(1u, br.counters.restarts);

    /* Restarted once only. */
    TEST_ASSERT_FALSE(step(1020u).restart);
}

/* ---- the latch ---- */

/* A bus-off seen at at_ms, its restart 10 ms later, and the controller
 * rejoined on a step 1 ms after that: clean from at_ms + 11. */
static void bus_off_and_restart(uint32_t at_ms)
{
    in.bus_off_count++;
    TEST_ASSERT_TRUE(step(at_ms).bus_off);
    TEST_ASSERT_TRUE(step(at_ms + BUS_RECOVERY_RESTART_MS).restart);
    (void)step(at_ms + BUS_RECOVERY_RESTART_MS + 1u);
}

static void test_the_third_bus_off_without_a_clean_second_latches(void)
{
    reset();
    bus_off_and_restart(1000u);
    bus_off_and_restart(1100u);
    TEST_ASSERT_FALSE(br.latched);

    /* 999 ms clean since 1111: not a clean second. */
    in.bus_off_count++;
    TEST_ASSERT_TRUE(step(2110u).bus_off);
    TEST_ASSERT_TRUE(br.latched);
}

static void test_while_latched_the_bus_is_retried_once_per_second(void)
{
    reset();
    bus_off_and_restart(1000u);
    bus_off_and_restart(1020u);
    in.bus_off_count++;
    (void)step(1040u); /* latched */

    TEST_ASSERT_FALSE(step(1050u).restart);
    TEST_ASSERT_FALSE(step(2039u).restart);
    TEST_ASSERT_TRUE(step(2040u).restart);

    /* It fails again at once: still latched, next retry a second later. */
    in.bus_off_count++;
    TEST_ASSERT_TRUE(step(2050u).bus_off);
    TEST_ASSERT_TRUE(br.latched);
    TEST_ASSERT_FALSE(step(3049u).restart);
    TEST_ASSERT_TRUE(step(3050u).restart);
}

static void test_a_clean_second_starts_the_count_again(void)
{
    reset();
    bus_off_and_restart(1000u);
    bus_off_and_restart(1100u);

    /* Exactly 1 s clean since 1111: a clean second. This is the first
     * bus-off of a new run, so two more are needed to latch. */
    in.bus_off_count++;
    (void)step(2111u);
    TEST_ASSERT_FALSE(br.latched);
    TEST_ASSERT_TRUE(step(2121u).restart);
    (void)step(2122u);

    bus_off_and_restart(2200u);
    TEST_ASSERT_FALSE(br.latched);
    in.bus_off_count++;
    (void)step(2300u);
    TEST_ASSERT_TRUE(br.latched);
}

static void test_two_bus_offs_between_steps_count_as_two(void)
{
    reset();
    bus_off_and_restart(1000u);

    /* The bus failed twice before the step looked: that is the third. */
    in.bus_off_count += 2u;
    TEST_ASSERT_TRUE(step(1100u).bus_off);
    TEST_ASSERT_EQUAL_UINT32(3u, br.counters.bus_offs);
    TEST_ASSERT_TRUE(br.latched);
}

/* Latched, with the slow restart at 2040; the controller has not yet
 * been seen rejoined. */
static void latch(void)
{
    reset();
    bus_off_and_restart(1000u);
    bus_off_and_restart(1020u);
    in.bus_off_count++;
    (void)step(1040u);
    TEST_ASSERT_TRUE(step(2040u).restart);
}

static void test_the_latch_clears_after_5_s_clean(void)
{
    latch();
    (void)step(2041u); /* rejoined */

    (void)step(7040u);
    TEST_ASSERT_TRUE(br.latched);
    (void)step(7041u);
    TEST_ASSERT_FALSE(br.latched);

    /* And a new run needs three bus-offs again. */
    bus_off_and_restart(7100u);
    TEST_ASSERT_FALSE(br.latched);
}

static void test_the_latch_waits_for_the_images_own_condition(void)
{
    latch();
    (void)step(2041u); /* rejoined */

    in.latch_may_clear = false;
    (void)step(9000u);
    TEST_ASSERT_TRUE(br.latched);

    in.latch_may_clear = true;
    (void)step(9010u);
    TEST_ASSERT_FALSE(br.latched);
}

static void test_clean_time_starts_when_the_controller_rejoins_the_bus(void)
{
    latch();

    /* Restarted at 2040, but the bus is held dominant: the controller
     * never finishes its recovery, so none of this is clean. */
    in.controller_off = true;
    (void)step(2050u);
    (void)step(9000u);
    TEST_ASSERT_TRUE(br.latched);

    /* It rejoins at 9000: clean from there. */
    in.controller_off = false;
    (void)step(9010u);
    (void)step(14009u);
    TEST_ASSERT_TRUE(br.latched);
    (void)step(14010u);
    TEST_ASSERT_FALSE(br.latched);
}

static void test_the_clean_second_and_the_unlatch_are_unaffected_by_the_wrap(void)
{
    reset();
    bus_off_and_restart(0xFFFFFF00u);
    bus_off_and_restart(0xFFFFFF80u);
    in.bus_off_count++;
    (void)step(0x00000010u); /* 133 ms clean, across the wrap: the third */
    TEST_ASSERT_TRUE(br.latched);

    TEST_ASSERT_TRUE(step(0x00000010u + BUS_RECOVERY_SLOW_RESTART_MS).restart);
    (void)step(0x00000010u + BUS_RECOVERY_SLOW_RESTART_MS + 10u); /* running */
    (void)step(0x00000010u + BUS_RECOVERY_SLOW_RESTART_MS + 10u + BUS_RECOVERY_UNLATCH_MS);
    TEST_ASSERT_FALSE(br.latched);
}

/* ---- error-passive ---- */

static void test_error_passive_is_reported_and_counted_and_changes_nothing_else(void)
{
    reset();
    in.error_passive = true;
    const bus_recovery_action_t a = step(1000u);

    TEST_ASSERT_TRUE(br.error_passive);
    TEST_ASSERT_FALSE(a.bus_off);
    TEST_ASSERT_FALSE(a.restart);
    TEST_ASSERT_FALSE(br.off);
    TEST_ASSERT_FALSE(br.latched);

    /* Counted once per entry, not per step. */
    (void)step(1010u);
    in.error_passive = false;
    (void)step(1020u);
    TEST_ASSERT_FALSE(br.error_passive);
    in.error_passive = true;
    (void)step(1030u);
    TEST_ASSERT_EQUAL_UINT32(2u, br.counters.error_passives);
}

/* ---- time ---- */

static void test_the_restart_is_unaffected_by_the_millisecond_count_wrapping(void)
{
    reset();
    in.bus_off_count = 1u;
    (void)step(0xFFFFFFFAu);

    TEST_ASSERT_FALSE(step(0x00000003u).restart);
    TEST_ASSERT_TRUE(step(0x00000004u).restart);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_bus_off_is_reported_in_the_step_that_sees_it);
    RUN_TEST(test_the_controller_is_restarted_10_ms_after_the_bus_off);
    RUN_TEST(test_the_third_bus_off_without_a_clean_second_latches);
    RUN_TEST(test_while_latched_the_bus_is_retried_once_per_second);
    RUN_TEST(test_a_clean_second_starts_the_count_again);
    RUN_TEST(test_two_bus_offs_between_steps_count_as_two);
    RUN_TEST(test_the_latch_clears_after_5_s_clean);
    RUN_TEST(test_the_latch_waits_for_the_images_own_condition);
    RUN_TEST(test_clean_time_starts_when_the_controller_rejoins_the_bus);
    RUN_TEST(test_the_clean_second_and_the_unlatch_are_unaffected_by_the_wrap);
    RUN_TEST(test_error_passive_is_reported_and_counted_and_changes_nothing_else);
    RUN_TEST(test_the_restart_is_unaffected_by_the_millisecond_count_wrapping);
    return UNITY_END();
}
