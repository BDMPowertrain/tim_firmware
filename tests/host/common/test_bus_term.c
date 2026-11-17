/*
 * Host tests for bus termination (App/common/logic/Inc/bus_term.h).
 *
 * Issue 52: "load the parameter, drop standby, write termination, and
 * start CAN1 at least 5 ms later"; "The manual termination override in the
 * shell still works for bench use and does not change the stored value."
 */
#include "bus_term.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_writing_the_stored_value_commands_the_relay_to_it(void)
{
    bus_term_t on = {0};
    bus_term_t off = {0};

    bus_term_write_stored(&on, true, 100u);
    bus_term_write_stored(&off, false, 100u);

    TEST_ASSERT_TRUE(on.relay);
    TEST_ASSERT_FALSE(off.relay);
    TEST_ASSERT_FALSE(bus_term_overridden(&on));
    TEST_ASSERT_FALSE(bus_term_overridden(&off));
}

static void test_a_bus_never_written_is_not_settled(void)
{
    const bus_term_t t = {0};

    TEST_ASSERT_FALSE(bus_term_settled(&t, 0u));
    TEST_ASSERT_FALSE(bus_term_settled(&t, 1000000u));
}

/* "At least 5 ms": with a 1 ms tick, a 5-tick gap can be just over 4 ms,
   so 6 ticks is the first that is surely 5 ms. */
static void test_the_bus_settles_six_ticks_after_the_write(void)
{
    bus_term_t t = {0};

    bus_term_write_stored(&t, false, 1000u);

    TEST_ASSERT_FALSE(bus_term_settled(&t, 1000u));
    TEST_ASSERT_FALSE(bus_term_settled(&t, 1005u));
    TEST_ASSERT_TRUE(bus_term_settled(&t, 1006u));
    TEST_ASSERT_TRUE(bus_term_settled(&t, 1010u));
}

static void test_settling_counts_across_the_tick_wrap(void)
{
    bus_term_t t = {0};

    bus_term_write_stored(&t, true, 0xFFFFFFFEu);

    TEST_ASSERT_FALSE(bus_term_settled(&t, 3u));
    TEST_ASSERT_TRUE(bus_term_settled(&t, 4u));
}

static void test_the_override_moves_the_relay_but_not_the_stored_value(void)
{
    bus_term_t t = {0};

    bus_term_write_stored(&t, false, 0u);
    bus_term_override(&t, true);

    TEST_ASSERT_TRUE(t.relay);
    TEST_ASSERT_FALSE(t.stored);
    TEST_ASSERT_TRUE(bus_term_overridden(&t));
}

static void test_overriding_back_to_the_stored_value_is_no_longer_an_override(void)
{
    bus_term_t t = {0};

    bus_term_write_stored(&t, true, 0u);
    bus_term_override(&t, false);
    bus_term_override(&t, true);

    TEST_ASSERT_TRUE(t.relay);
    TEST_ASSERT_TRUE(t.stored);
    TEST_ASSERT_FALSE(bus_term_overridden(&t));
}

/* Settling is a start-up matter: an override on the bench does not make
   a settled bus unsettled again. */
static void test_an_override_does_not_restart_settling(void)
{
    bus_term_t t = {0};

    bus_term_write_stored(&t, false, 0u);
    bus_term_override(&t, true);

    TEST_ASSERT_TRUE(bus_term_settled(&t, 6u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_writing_the_stored_value_commands_the_relay_to_it);
    RUN_TEST(test_a_bus_never_written_is_not_settled);
    RUN_TEST(test_the_bus_settles_six_ticks_after_the_write);
    RUN_TEST(test_settling_counts_across_the_tick_wrap);
    RUN_TEST(test_the_override_moves_the_relay_but_not_the_stored_value);
    RUN_TEST(test_overriding_back_to_the_stored_value_is_no_longer_an_override);
    RUN_TEST(test_an_override_does_not_restart_settling);
    return UNITY_END();
}
