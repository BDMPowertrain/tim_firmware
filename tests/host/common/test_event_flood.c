/*
 * Host tests for the Event log's flood guard
 * (App/common/logic/Inc/event_flood.h): a burst of 64, refilled at one per
 * second, with one "N events dropped" report.
 */
#include "event_flood.h"
#include "unity.h"

static event_flood_t f;

void setUp(void)
{
    event_flood_init(&f, 5000u);
}

void tearDown(void) {}

/* Admits n Events at now_ms; returns how many got through. */
static uint32_t offer(uint32_t n, uint32_t now_ms)
{
    uint32_t admitted = 0u;

    for (uint32_t i = 0; i < n; i++) {
        admitted += event_flood_admit(&f, now_ms) ? 1u : 0u;
    }
    return admitted;
}

static void test_a_burst_of_64_passes_and_the_65th_is_dropped(void)
{
    TEST_ASSERT_EQUAL_UINT32(64u, offer(64u, 5000u));
    TEST_ASSERT_FALSE(event_flood_admit(&f, 5000u));
}

static void test_one_token_returns_each_second(void)
{
    (void)offer(65u, 5000u);
    TEST_ASSERT_FALSE(event_flood_admit(&f, 5999u));
    TEST_ASSERT_TRUE(event_flood_admit(&f, 6000u));
    TEST_ASSERT_FALSE(event_flood_admit(&f, 6000u));
    /* 2.5 s later: two tokens, the half second carried to the next. */
    TEST_ASSERT_EQUAL_UINT32(2u, offer(5u, 8500u));
    TEST_ASSERT_TRUE(event_flood_admit(&f, 9000u));
}

static void test_the_refill_stops_at_a_full_burst(void)
{
    (void)offer(64u, 5000u);
    TEST_ASSERT_EQUAL_UINT32(64u, offer(100u, 5000u + 3600000u));
}

static void test_the_refill_survives_the_millisecond_wrap(void)
{
    event_flood_init(&f, 0xFFFFFE00u);
    (void)offer(64u, 0xFFFFFE00u);
    TEST_ASSERT_TRUE(event_flood_admit(&f, 0xFFFFFE00u + 1000u));
}

static void test_drops_are_reported_once_when_a_token_returns(void)
{
    (void)offer(64u + 10u, 5000u);
    TEST_ASSERT_EQUAL_UINT32(0u, event_flood_take_report(&f, 5500u)); /* no token yet */
    TEST_ASSERT_EQUAL_UINT32(10u, event_flood_take_report(&f, 6000u));
    TEST_ASSERT_EQUAL_UINT32(0u, event_flood_take_report(&f, 6000u));
    /* The report took no token: the next Event still gets in. */
    TEST_ASSERT_TRUE(event_flood_admit(&f, 6000u));
}

static void test_nothing_dropped_is_nothing_reported(void)
{
    (void)offer(3u, 5000u);
    TEST_ASSERT_EQUAL_UINT32(0u, event_flood_take_report(&f, 9000u));
}

static void test_queue_drops_join_the_report(void)
{
    event_flood_add_dropped(&f, 4u);
    TEST_ASSERT_EQUAL_UINT32(4u, event_flood_take_report(&f, 5000u));

    (void)offer(64u + 2u, 5000u);
    event_flood_add_dropped(&f, 3u);
    TEST_ASSERT_EQUAL_UINT32(5u, event_flood_take_report(&f, 6000u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_burst_of_64_passes_and_the_65th_is_dropped);
    RUN_TEST(test_one_token_returns_each_second);
    RUN_TEST(test_the_refill_stops_at_a_full_burst);
    RUN_TEST(test_the_refill_survives_the_millisecond_wrap);
    RUN_TEST(test_drops_are_reported_once_when_a_token_returns);
    RUN_TEST(test_nothing_dropped_is_nothing_reported);
    RUN_TEST(test_queue_drops_join_the_report);
    return UNITY_END();
}
