/*
 * Host tests for the reset record (App/common/logic/Inc/reset_record.h):
 * the Breadcrumb, the Crash record and the count of resets that were not
 * a Power-on reset, as kept in the backup registers across a reset.
 *
 * Issue 49: "The crash record holds cause, task and fault address; a count
 * of non-power-on resets is kept; all are cleared on a power-on reset."
 * Spec (#36), Watchdog and crash path: "Breadcrumb (step count, phase,
 * overrun count) and crash record (cause, task, fault address) live in the
 * backup registers with a count of non-power-on resets; all are cleared on
 * a power-on reset."
 *
 * Each test plays the registers through the module's own interface: boot,
 * then write what the running image writes, then boot again.
 */
#include <string.h>

#include "reset_record.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* The backup registers after a backup-domain reset: all zero. */
static void erased(uint32_t words[RESET_RECORD_WORDS])
{
    memset(words, 0, RESET_RECORD_WORDS * sizeof(words[0]));
}

static void power_on(uint32_t words[RESET_RECORD_WORDS])
{
    reset_report_t report;

    erased(words);
    reset_record_boot(words, RESET_CAUSE_POWER_ON, &report);
}

static void test_a_power_on_reset_reports_nothing(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;

    erased(words);
    reset_record_boot(words, RESET_CAUSE_POWER_ON, &report);

    TEST_ASSERT_EQUAL(RESET_CAUSE_POWER_ON, report.cause);
    TEST_ASSERT_EQUAL_UINT32(0u, report.non_power_on_resets);
    TEST_ASSERT_EQUAL_UINT8(BREADCRUMB_PHASE_NONE, report.breadcrumb.phase);
    TEST_ASSERT_EQUAL(CRASH_NONE, report.crash.cause);
}

static void test_a_watchdog_reset_reports_the_breadcrumb_left_before_it(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;
    const breadcrumb_t left = { .step_count = 123456u, .phase = 2u, .overruns = 7u };

    power_on(words);
    reset_record_put_breadcrumb(words, &left);
    reset_record_boot(words, RESET_CAUSE_WATCHDOG, &report);

    TEST_ASSERT_EQUAL(RESET_CAUSE_WATCHDOG, report.cause);
    TEST_ASSERT_TRUE(report.layout_found);
    TEST_ASSERT_EQUAL_UINT32(123456u, report.breadcrumb.step_count);
    TEST_ASSERT_EQUAL_UINT8(2u, report.breadcrumb.phase);
    TEST_ASSERT_EQUAL_UINT32(7u, report.breadcrumb.overruns);
    TEST_ASSERT_EQUAL(CRASH_NONE, report.crash.cause);
}

static void crash(uint32_t words[RESET_RECORD_WORDS], crash_cause_t cause, const char *task,
                  uint32_t address)
{
    crash_record_t record = { .cause = cause, .address = address };

    strncpy(record.task, task, CRASH_TASK_CHARS);
    reset_record_put_crash(words, &record);
}

static void test_a_crash_reports_cause_task_and_address(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;

    power_on(words);
    crash(words, CRASH_HARD_FAULT, "shell", 0x08001234u);
    reset_record_boot(words, RESET_CAUSE_SOFTWARE, &report);

    TEST_ASSERT_EQUAL(RESET_CAUSE_SOFTWARE, report.cause);
    TEST_ASSERT_EQUAL(CRASH_HARD_FAULT, report.crash.cause);
    TEST_ASSERT_EQUAL_STRING("shell", report.crash.task);
    TEST_ASSERT_EQUAL_HEX32(0x08001234u, report.crash.address);
}

static void test_a_long_task_name_is_cut_to_eight_characters(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;
    crash_record_t record = { .cause = CRASH_STACK_OVERFLOW };

    power_on(words);
    memcpy(record.task, "controlS", CRASH_TASK_CHARS); /* "controlStep", cut */
    reset_record_put_crash(words, &record);
    reset_record_boot(words, RESET_CAUSE_SOFTWARE, &report);

    TEST_ASSERT_EQUAL_STRING("controlS", report.crash.task);
}

/* The Breadcrumb and the Crash record are written separately and neither
   disturbs the other: a crash mid-step reports both. */
static void test_a_crash_mid_step_reports_both_records(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;
    const breadcrumb_t left = { .step_count = 9u, .phase = 0u, .overruns = 0u };

    power_on(words);
    reset_record_put_breadcrumb(words, &left);
    crash(words, CRASH_ASSERT, "controlS", 0x08000100u);
    reset_record_boot(words, RESET_CAUSE_SOFTWARE, &report);

    TEST_ASSERT_EQUAL_UINT32(9u, report.breadcrumb.step_count);
    TEST_ASSERT_EQUAL_UINT8(0u, report.breadcrumb.phase);
    TEST_ASSERT_EQUAL(CRASH_ASSERT, report.crash.cause);
}

static void test_the_special_phases_survive_the_reset(void)
{
    const uint8_t phases[] = { BREADCRUMB_PHASE_START, BREADCRUMB_PHASE_END, 0u, 15u };

    for (unsigned i = 0; i < sizeof(phases); i++) {
        uint32_t words[RESET_RECORD_WORDS];
        reset_report_t report;
        const breadcrumb_t left = { .step_count = 1u, .phase = phases[i] };

        power_on(words);
        reset_record_put_breadcrumb(words, &left);
        reset_record_boot(words, RESET_CAUSE_WATCHDOG, &report);

        TEST_ASSERT_EQUAL_UINT8(phases[i], report.breadcrumb.phase);
    }
}

/* A watchdog reset before the first step: the step never wrote, so there
   is no Breadcrumb to report. */
static void test_a_reset_before_the_first_step_reports_no_breadcrumb(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;

    power_on(words);
    reset_record_boot(words, RESET_CAUSE_WATCHDOG, &report);

    TEST_ASSERT_TRUE(report.layout_found);
    TEST_ASSERT_EQUAL_UINT8(BREADCRUMB_PHASE_NONE, report.breadcrumb.phase);
    TEST_ASSERT_EQUAL(CRASH_NONE, report.crash.cause);
}

static void test_each_reset_that_is_not_power_on_counts_one(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;
    const reset_cause_t causes[] = { RESET_CAUSE_WATCHDOG, RESET_CAUSE_SOFTWARE,
                                     RESET_CAUSE_PIN, RESET_CAUSE_OPTION_LOAD,
                                     RESET_CAUSE_UNKNOWN };

    power_on(words);
    for (unsigned i = 0; i < sizeof(causes) / sizeof(causes[0]); i++) {
        reset_record_boot(words, causes[i], &report);
        TEST_ASSERT_EQUAL_UINT32(i + 1u, report.non_power_on_resets);
    }
}

/* Issue 49: "all are cleared on a power-on reset". */
static void test_a_power_on_reset_clears_the_count_and_both_records(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;
    const breadcrumb_t left = { .step_count = 50u, .phase = 1u, .overruns = 3u };

    power_on(words);
    reset_record_boot(words, RESET_CAUSE_WATCHDOG, &report);
    reset_record_put_breadcrumb(words, &left);
    crash(words, CRASH_BUS_FAULT, "daqTask", 0x08004000u);

    /* The words as found, not erased: the backup domain is reset on a
       Power-on reset too, but the rule holds without it. */
    reset_record_boot(words, RESET_CAUSE_POWER_ON, &report);
    TEST_ASSERT_EQUAL_UINT32(0u, report.non_power_on_resets);
    TEST_ASSERT_EQUAL_UINT8(BREADCRUMB_PHASE_NONE, report.breadcrumb.phase);
    TEST_ASSERT_EQUAL(CRASH_NONE, report.crash.cause);

    reset_record_boot(words, RESET_CAUSE_WATCHDOG, &report);
    TEST_ASSERT_EQUAL_UINT32(1u, report.non_power_on_resets);
    TEST_ASSERT_EQUAL_UINT8(BREADCRUMB_PHASE_NONE, report.breadcrumb.phase);
    TEST_ASSERT_EQUAL(CRASH_NONE, report.crash.cause);
}

/* A record is reported against the one reset that followed it: a crash,
   then a watchdog reset during start-up, does not show the crash twice. */
static void test_a_record_is_reported_once(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;
    const breadcrumb_t left = { .step_count = 4u, .phase = 1u };

    power_on(words);
    reset_record_put_breadcrumb(words, &left);
    crash(words, CRASH_USAGE_FAULT, "imuTask", 0x08002000u);
    reset_record_boot(words, RESET_CAUSE_SOFTWARE, &report);
    TEST_ASSERT_EQUAL(CRASH_USAGE_FAULT, report.crash.cause);

    reset_record_boot(words, RESET_CAUSE_WATCHDOG, &report);
    TEST_ASSERT_EQUAL(CRASH_NONE, report.crash.cause);
    TEST_ASSERT_EQUAL_UINT8(BREADCRUMB_PHASE_NONE, report.breadcrumb.phase);
    TEST_ASSERT_EQUAL_UINT32(2u, report.non_power_on_resets);
}

/* Words not in this layout (left by other code, or by a backup domain that
   missed its reset, ES0430 2.2.5) are not reported as ours. */
static void test_words_in_another_layout_are_not_reported(void)
{
    uint32_t words[RESET_RECORD_WORDS];
    reset_report_t report;

    for (unsigned i = 0; i < RESET_RECORD_WORDS; i++) {
        words[i] = 0xA5A5A5A5u;
    }
    reset_record_boot(words, RESET_CAUSE_PIN, &report);

    TEST_ASSERT_FALSE(report.layout_found);
    TEST_ASSERT_EQUAL_UINT32(1u, report.non_power_on_resets);
    TEST_ASSERT_EQUAL_UINT8(BREADCRUMB_PHASE_NONE, report.breadcrumb.phase);
    TEST_ASSERT_EQUAL(CRASH_NONE, report.crash.cause);

    /* From then on they are ours. */
    reset_record_boot(words, RESET_CAUSE_PIN, &report);
    TEST_ASSERT_TRUE(report.layout_found);
    TEST_ASSERT_EQUAL_UINT32(2u, report.non_power_on_resets);
}

static void test_every_crash_cause_has_a_name(void)
{
    TEST_ASSERT_EQUAL_STRING("none", crash_cause_name(CRASH_NONE));
    TEST_ASSERT_EQUAL_STRING("stack overflow", crash_cause_name(CRASH_STACK_OVERFLOW));
    TEST_ASSERT_EQUAL_STRING("crash command", crash_cause_name(CRASH_COMMAND));
    TEST_ASSERT_EQUAL_STRING("?", crash_cause_name((crash_cause_t)99));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_power_on_reset_reports_nothing);
    RUN_TEST(test_a_watchdog_reset_reports_the_breadcrumb_left_before_it);
    RUN_TEST(test_a_crash_reports_cause_task_and_address);
    RUN_TEST(test_a_long_task_name_is_cut_to_eight_characters);
    RUN_TEST(test_a_crash_mid_step_reports_both_records);
    RUN_TEST(test_the_special_phases_survive_the_reset);
    RUN_TEST(test_a_reset_before_the_first_step_reports_no_breadcrumb);
    RUN_TEST(test_each_reset_that_is_not_power_on_counts_one);
    RUN_TEST(test_a_power_on_reset_clears_the_count_and_both_records);
    RUN_TEST(test_a_record_is_reported_once);
    RUN_TEST(test_words_in_another_layout_are_not_reported);
    RUN_TEST(test_every_crash_cause_has_a_name);
    return UNITY_END();
}
