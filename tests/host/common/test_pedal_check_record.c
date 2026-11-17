/*
 * Host tests for finding a Pedal check pass in the Event log (issue 59,
 * ADR 0016), on the RAM-backed flash adapter: Seam 2 of spec #36, "the
 * pedal-check Event match".
 *
 * ADR 0016: the pass is written as an Event carrying the image's build ID
 * and the parameter save counter; a check is due whenever no such Event
 * matches the running image and the loaded parameters. If the Event log
 * has lost it, the check is asked for again. cmake/build_id_write.cmake: a
 * dirty build must be treated as never matching.
 */
#include <string.h>

#include "event_codes.h"
#include "event_log.h"
#include "ram_flash.h"
#include "unity.h"

#define BUILD   0x1234ABCDu
#define COUNTER 7u

static event_log_t log_;

void setUp(void)
{
    ram_flash_reset();
    event_log_open(&log_, ram_flash());
}

void tearDown(void) {}

static void reset(void)
{
    ram_flash_power_on();
    event_log_open(&log_, ram_flash());
}

static void append(uint16_t code, const uint8_t data[EVENT_DATA_SIZE])
{
    TEST_ASSERT_EQUAL(EVENT_APPEND_OK, event_log_append(&log_, code, 1000u, data));
}

static void append_pass(uint32_t build, uint32_t counter, bool dirty)
{
    uint8_t data[EVENT_DATA_SIZE];

    event_data_pedal_check(data, build, counter, dirty);
    append(EVENT_PEDAL_CHECK, data);
}

static void append_other(uint32_t n)
{
    uint8_t data[EVENT_DATA_SIZE] = { 0 };

    for (uint32_t i = 0; i < n; i++) {
        append(EVENT_TORQUE_CUT, data);
    }
}

/* The search the VCU makes at start-up. */
static bool found(uint32_t build, uint32_t counter, bool dirty)
{
    const event_pedal_check_key_t key = { build, counter, dirty };
    event_t ev;

    return event_log_find(&log_, event_pedal_check_matches, &key, &ev);
}

static void test_a_pass_for_this_image_and_these_parameters_is_found_after_a_reset(void)
{
    append_pass(BUILD, COUNTER, false);
    append_other(100u);
    reset();
    append_other(5u);
    TEST_ASSERT_TRUE(found(BUILD, COUNTER, false));
}

static void test_the_search_gives_the_event_it_found(void)
{
    const event_pedal_check_key_t key = { BUILD, COUNTER, false };
    event_t ev;

    append_other(3u);
    append_pass(BUILD, COUNTER, false);
    TEST_ASSERT_TRUE(event_log_find(&log_, event_pedal_check_matches, &key, &ev));
    TEST_ASSERT_EQUAL_UINT16(EVENT_PEDAL_CHECK, ev.code);
    TEST_ASSERT_EQUAL_UINT32(3u, ev.seq);
}

/* A new image is flashed: the check is due again. */
static void test_a_pass_for_another_build_is_not_found(void)
{
    append_pass(BUILD, COUNTER, false);
    TEST_ASSERT_FALSE(found(BUILD + 1u, COUNTER, false));
}

/* A new calibration was saved: the check is due again. */
static void test_a_pass_for_another_save_counter_is_not_found(void)
{
    append_pass(BUILD, COUNTER, false);
    TEST_ASSERT_FALSE(found(BUILD, COUNTER + 1u, false));
}

/* An older pass for these values still counts. */
static void test_an_older_matching_pass_is_found_behind_newer_ones(void)
{
    append_pass(BUILD, COUNTER, false);
    append_pass(BUILD, COUNTER + 1u, false);
    append_pass(BUILD + 1u, COUNTER, false);
    TEST_ASSERT_TRUE(found(BUILD, COUNTER, false));
}

static void test_a_dirty_build_never_matches(void)
{
    append_pass(BUILD, COUNTER, true);
    append_pass(BUILD, COUNTER, false);
    TEST_ASSERT_FALSE(found(BUILD, COUNTER, true));
}

static void test_a_pass_a_dirty_build_wrote_never_matches(void)
{
    append_pass(BUILD, COUNTER, true);
    TEST_ASSERT_FALSE(found(BUILD, COUNTER, false));
}

static void test_a_build_with_no_id_never_matches(void)
{
    append_pass(0u, COUNTER, false);
    TEST_ASSERT_FALSE(found(0u, COUNTER, false));
}

/* Another Event with the same bytes is not a pass. */
static void test_only_a_pedal_check_event_matches(void)
{
    uint8_t data[EVENT_DATA_SIZE];

    event_data_pedal_check(data, BUILD, COUNTER, false);
    append(EVENT_PEDAL_CHECK_FAILED, data);
    TEST_ASSERT_FALSE(found(BUILD, COUNTER, false));
}

/* ADR 0016: a lost Event log asks for the check again. */
static void test_a_lost_event_log_asks_for_the_check_again(void)
{
    append_pass(BUILD, COUNTER, false);
    TEST_ASSERT_TRUE(found(BUILD, COUNTER, false));

    ram_flash_reset(); /* bank 2 erased */
    reset();
    TEST_ASSERT_FALSE(found(BUILD, COUNTER, false));
}

/* The ring wraps over the pass: lost the same way. */
static void test_a_pass_the_ring_has_wrapped_over_is_not_found(void)
{
    append_pass(BUILD, COUNTER, false);
    append_other(EVENT_LOG_PAGES * EVENT_LOG_SLOTS - 100u);
    TEST_ASSERT_TRUE(found(BUILD, COUNTER, false)); /* not yet */
    append_other(100u);
    TEST_ASSERT_FALSE(found(BUILD, COUNTER, false));
}

/* A pass that no longer reads whole is skipped, not trusted. */
static void test_a_corrupt_pass_is_not_found(void)
{
    append_pass(BUILD, COUNTER, false);
    TEST_ASSERT_TRUE(found(BUILD, COUNTER, false));
    ram_flash_flip(EVENT_LOG_ADDR + EVENT_SIZE + 12u, 0x01u); /* its build ID */
    reset();
    TEST_ASSERT_FALSE(found(BUILD, COUNTER, false));
}

static void test_an_empty_log_or_no_flash_finds_nothing(void)
{
    TEST_ASSERT_FALSE(found(BUILD, COUNTER, false));

    event_log_open(&log_, NULL);
    TEST_ASSERT_FALSE(found(BUILD, COUNTER, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_pass_for_this_image_and_these_parameters_is_found_after_a_reset);
    RUN_TEST(test_the_search_gives_the_event_it_found);
    RUN_TEST(test_a_pass_for_another_build_is_not_found);
    RUN_TEST(test_a_pass_for_another_save_counter_is_not_found);
    RUN_TEST(test_an_older_matching_pass_is_found_behind_newer_ones);
    RUN_TEST(test_a_dirty_build_never_matches);
    RUN_TEST(test_a_pass_a_dirty_build_wrote_never_matches);
    RUN_TEST(test_a_build_with_no_id_never_matches);
    RUN_TEST(test_only_a_pedal_check_event_matches);
    RUN_TEST(test_a_lost_event_log_asks_for_the_check_again);
    RUN_TEST(test_a_pass_the_ring_has_wrapped_over_is_not_found);
    RUN_TEST(test_a_corrupt_pass_is_not_found);
    RUN_TEST(test_an_empty_log_or_no_flash_finds_nothing);
    return UNITY_END();
}
