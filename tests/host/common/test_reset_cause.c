/*
 * Host tests for the reset cause (App/common/logic/Inc/reset_cause.h,
 * ADR 0011).
 *
 * ADR 0011: "Both images therefore treat 'BOR flag set' as a power-on
 * reset." The flag bits are RCC_CSR's (RM0440 7.4.28), written here as
 * literals from the manual so the test does not share the module's names.
 */
#include "reset_cause.h"
#include "unity.h"

/* RCC_CSR bits, RM0440 section 7.4.28. */
#define OBLRSTF  (1u << 25)
#define PINRSTF  (1u << 26)
#define BORRSTF  (1u << 27)
#define SFTRSTF  (1u << 28)
#define IWDGRSTF (1u << 29)
#define WWDGRSTF (1u << 30)
#define LPWRRSTF (1u << 31)

/* RCC_CSR's reset value: after a power-on both BOR and PIN are set. */
#define CSR_AFTER_POWER_ON 0x0C000000u

void setUp(void) {}
void tearDown(void) {}

static void test_the_value_after_switching_on_is_a_power_on_reset(void)
{
    TEST_ASSERT_EQUAL(RESET_CAUSE_POWER_ON, reset_cause_classify(CSR_AFTER_POWER_ON));
}

/* ADR 0011: a sag below the threshold sets the same flag; the flags also
   accumulate until cleared, so BOR wins over anything set with it. */
static void test_bor_with_any_other_flag_is_a_power_on_reset(void)
{
    const uint32_t others[] = { OBLRSTF, PINRSTF, SFTRSTF, IWDGRSTF, WWDGRSTF, LPWRRSTF };

    for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        TEST_ASSERT_EQUAL(RESET_CAUSE_POWER_ON, reset_cause_classify(BORRSTF | others[i]));
    }
}

/* An internal reset also pulses NRST, so PIN may come with any of them:
   the internal cause wins. */
static void test_an_internal_cause_wins_over_the_pin(void)
{
    TEST_ASSERT_EQUAL(RESET_CAUSE_WATCHDOG, reset_cause_classify(IWDGRSTF | PINRSTF));
    TEST_ASSERT_EQUAL(RESET_CAUSE_WINDOW_WATCHDOG, reset_cause_classify(WWDGRSTF | PINRSTF));
    TEST_ASSERT_EQUAL(RESET_CAUSE_SOFTWARE, reset_cause_classify(SFTRSTF | PINRSTF));
    TEST_ASSERT_EQUAL(RESET_CAUSE_OPTION_LOAD, reset_cause_classify(OBLRSTF | PINRSTF));
    TEST_ASSERT_EQUAL(RESET_CAUSE_LOW_POWER, reset_cause_classify(LPWRRSTF | PINRSTF));
}

static void test_the_pin_alone_is_a_pin_reset(void)
{
    TEST_ASSERT_EQUAL(RESET_CAUSE_PIN, reset_cause_classify(PINRSTF));
}

/* The watchdog is the cause that matters most after a hang: it wins over
   a software reset left uncleared alongside it. */
static void test_the_watchdog_wins_over_software(void)
{
    TEST_ASSERT_EQUAL(RESET_CAUSE_WATCHDOG, reset_cause_classify(IWDGRSTF | SFTRSTF));
}

static void test_no_flag_is_unknown(void)
{
    TEST_ASSERT_EQUAL(RESET_CAUSE_UNKNOWN, reset_cause_classify(0u));
    /* Bits outside the flags (LSI on, LSI ready) say nothing. */
    TEST_ASSERT_EQUAL(RESET_CAUSE_UNKNOWN, reset_cause_classify(0x00000003u));
}

static void test_only_a_power_on_reset_is_power_on(void)
{
    TEST_ASSERT_TRUE(reset_cause_is_power_on(RESET_CAUSE_POWER_ON));
    TEST_ASSERT_FALSE(reset_cause_is_power_on(RESET_CAUSE_WATCHDOG));
    TEST_ASSERT_FALSE(reset_cause_is_power_on(RESET_CAUSE_SOFTWARE));
    TEST_ASSERT_FALSE(reset_cause_is_power_on(RESET_CAUSE_PIN));
    TEST_ASSERT_FALSE(reset_cause_is_power_on(RESET_CAUSE_UNKNOWN));
}

static void test_every_cause_has_a_name(void)
{
    TEST_ASSERT_EQUAL_STRING("power-on", reset_cause_name(RESET_CAUSE_POWER_ON));
    TEST_ASSERT_EQUAL_STRING("watchdog", reset_cause_name(RESET_CAUSE_WATCHDOG));
    TEST_ASSERT_EQUAL_STRING("pin", reset_cause_name(RESET_CAUSE_PIN));
    TEST_ASSERT_EQUAL_STRING("?", reset_cause_name((reset_cause_t)99));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_value_after_switching_on_is_a_power_on_reset);
    RUN_TEST(test_bor_with_any_other_flag_is_a_power_on_reset);
    RUN_TEST(test_an_internal_cause_wins_over_the_pin);
    RUN_TEST(test_the_pin_alone_is_a_pin_reset);
    RUN_TEST(test_the_watchdog_wins_over_software);
    RUN_TEST(test_no_flag_is_unknown);
    RUN_TEST(test_only_a_power_on_reset_is_power_on);
    RUN_TEST(test_every_cause_has_a_name);
    return UNITY_END();
}
