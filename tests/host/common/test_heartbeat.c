/*
 * Host tests for the heartbeat LED's blink decision
 * (App/common/logic/Inc/heartbeat.h).
 *
 * Spec (issue 47): "The heartbeat LED blinks at 1 Hz for normal and 5 Hz for
 * any active fault, and toggles only if the control step's step count has
 * advanced since the last toggle." Issue 36, user story 41: "1 Hz for normal,
 * 5 Hz for any active fault and stopped for a stall".
 *
 * Housekeeping calls it every 100 ms with the boundary it woke on.
 */
#include "heartbeat.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Runs housekeeping from from_ms to to_ms (exclusive) in 100 ms ticks, the
 * step advancing by 10 every tick unless stalled. Returns the toggles. */
static uint32_t run(heartbeat_t *hb, uint32_t from_ms, uint32_t to_ms,
                    uint32_t *step_count, bool step_running, bool fault)
{
    uint32_t toggles = 0u;

    for (uint32_t t = from_ms; t != to_ms; t += 100u) {
        if (step_running) {
            *step_count += 10u;
        }
        if (heartbeat_update(hb, t, *step_count, fault)) {
            toggles++;
        }
    }
    return toggles;
}

static void test_normal_blinks_at_1_hz(void)
{
    heartbeat_t hb = {0};
    uint32_t steps = 0u;

    (void)run(&hb, 100u, 1100u, &steps, true, false); /* settle */
    /* 10 s at 1 Hz: 20 toggles, one every 500 ms. */
    TEST_ASSERT_EQUAL_UINT32(20u, run(&hb, 1100u, 11100u, &steps, true, false));
}

static void test_an_active_fault_blinks_at_5_hz(void)
{
    heartbeat_t hb = {0};
    uint32_t steps = 0u;

    (void)run(&hb, 100u, 1100u, &steps, true, true);
    /* 10 s at 5 Hz: 100 toggles, one every 100 ms. */
    TEST_ASSERT_EQUAL_UINT32(100u, run(&hb, 1100u, 11100u, &steps, true, true));
}

static void test_a_normal_toggle_comes_500_ms_after_the_last(void)
{
    heartbeat_t hb = {0};

    TEST_ASSERT_TRUE(heartbeat_update(&hb, 1000u, 100u, false));
    TEST_ASSERT_FALSE(heartbeat_update(&hb, 1100u, 110u, false));
    TEST_ASSERT_FALSE(heartbeat_update(&hb, 1400u, 140u, false));
    TEST_ASSERT_TRUE(heartbeat_update(&hb, 1500u, 150u, false));
}

static void test_a_fault_is_shown_on_the_next_tick(void)
{
    heartbeat_t hb = {0};

    TEST_ASSERT_TRUE(heartbeat_update(&hb, 1000u, 100u, false));
    /* The fault arrives 100 ms after a normal toggle: 5 Hz at once, not
       after the rest of the 500 ms half-period. */
    TEST_ASSERT_TRUE(heartbeat_update(&hb, 1100u, 110u, true));
    TEST_ASSERT_TRUE(heartbeat_update(&hb, 1200u, 120u, true));
}

static void test_a_cleared_fault_returns_to_1_hz(void)
{
    heartbeat_t hb = {0};
    uint32_t steps = 0u;

    (void)run(&hb, 100u, 2100u, &steps, true, true);
    TEST_ASSERT_EQUAL_UINT32(20u, run(&hb, 2100u, 12100u, &steps, true, false));
}

static void test_a_stalled_step_stops_the_led(void)
{
    heartbeat_t hb = {0};
    uint32_t steps = 0u;

    (void)run(&hb, 100u, 1100u, &steps, true, false);
    TEST_ASSERT_EQUAL_UINT32(0u, run(&hb, 1100u, 11100u, &steps, false, false));
    TEST_ASSERT_EQUAL_UINT32(0u, run(&hb, 11100u, 21100u, &steps, false, true));
}

static void test_the_led_stays_still_until_the_step_has_run(void)
{
    heartbeat_t hb = {0};

    /* The snapshot reads step count 0 until the first step has run. */
    for (uint32_t t = 100u; t <= 3000u; t += 100u) {
        TEST_ASSERT_FALSE(heartbeat_update(&hb, t, 0u, false));
    }
    TEST_ASSERT_TRUE(heartbeat_update(&hb, 3100u, 1u, false));
}

static void test_a_resumed_step_blinks_on_the_next_tick(void)
{
    heartbeat_t hb = {0};

    TEST_ASSERT_TRUE(heartbeat_update(&hb, 1000u, 100u, false));
    TEST_ASSERT_FALSE(heartbeat_update(&hb, 2000u, 100u, false)); /* stalled */
    TEST_ASSERT_TRUE(heartbeat_update(&hb, 2100u, 101u, false));
}

static void test_one_step_between_toggles_is_enough(void)
{
    heartbeat_t hb = {0};

    TEST_ASSERT_TRUE(heartbeat_update(&hb, 1000u, 7u, true));
    TEST_ASSERT_TRUE(heartbeat_update(&hb, 1100u, 8u, true));
    TEST_ASSERT_FALSE(heartbeat_update(&hb, 1200u, 8u, true));
}

static void test_the_millisecond_count_wrapping_changes_nothing(void)
{
    heartbeat_t hb = {0};
    uint32_t steps = 0u;
    const uint32_t start = 0u - 5000u; /* 5 s before the wrap */

    (void)run(&hb, start, start + 1000u, &steps, true, false);
    TEST_ASSERT_EQUAL_UINT32(20u, run(&hb, start + 1000u, start + 11000u, &steps, true, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normal_blinks_at_1_hz);
    RUN_TEST(test_an_active_fault_blinks_at_5_hz);
    RUN_TEST(test_a_normal_toggle_comes_500_ms_after_the_last);
    RUN_TEST(test_a_fault_is_shown_on_the_next_tick);
    RUN_TEST(test_a_cleared_fault_returns_to_1_hz);
    RUN_TEST(test_a_stalled_step_stops_the_led);
    RUN_TEST(test_the_led_stays_still_until_the_step_has_run);
    RUN_TEST(test_a_resumed_step_blinks_on_the_next_tick);
    RUN_TEST(test_one_step_between_toggles_is_enough);
    RUN_TEST(test_the_millisecond_count_wrapping_changes_nothing);
    return UNITY_END();
}
