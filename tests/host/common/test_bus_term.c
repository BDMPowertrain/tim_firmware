/*
 * Host tests for bus termination (App/common/logic/Inc/bus_term.h).
 *
 * Issue 52: "CAN termination per bus becomes a stored parameter, default
 * open"; "The manual termination override in the shell still works for
 * bench use and does not change the stored value."
 */
#include "bus_term.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_writing_the_stored_value_commands_the_relay_to_it(void)
{
    bus_term_t on = {0};
    bus_term_t off = {0};

    bus_term_write_stored(&on, true);
    bus_term_write_stored(&off, false);

    TEST_ASSERT_TRUE(on.relay);
    TEST_ASSERT_FALSE(off.relay);
    TEST_ASSERT_TRUE(on.written);
    TEST_ASSERT_FALSE(bus_term_overridden(&on));
    TEST_ASSERT_FALSE(bus_term_overridden(&off));
}

static void test_the_override_moves_the_relay_but_not_the_stored_value(void)
{
    bus_term_t t = {0};

    bus_term_write_stored(&t, false);
    bus_term_override(&t, true);

    TEST_ASSERT_TRUE(t.relay);
    TEST_ASSERT_FALSE(t.stored);
    TEST_ASSERT_TRUE(bus_term_overridden(&t));
}

static void test_overriding_back_to_the_stored_value_is_no_longer_an_override(void)
{
    bus_term_t t = {0};

    bus_term_write_stored(&t, true);
    bus_term_override(&t, false);
    bus_term_override(&t, true);

    TEST_ASSERT_TRUE(t.relay);
    TEST_ASSERT_TRUE(t.stored);
    TEST_ASSERT_FALSE(bus_term_overridden(&t));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_writing_the_stored_value_commands_the_relay_to_it);
    RUN_TEST(test_the_override_moves_the_relay_but_not_the_stored_value);
    RUN_TEST(test_overriding_back_to_the_stored_value_is_no_longer_an_override);
    return UNITY_END();
}
