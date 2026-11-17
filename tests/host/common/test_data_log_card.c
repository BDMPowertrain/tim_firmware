/*
 * Host tests for the Data log's card handling (App/common/logic/Inc/data_log_card.h).
 *
 * Issue 53: "One directory per boot number, a new file each boot, rolled
 * every 10 minutes"; "syncs every 1 s"; "Missing, removed or write error:
 * close, set a status bit, write one Event, retry the mount every 5 s into
 * a new file"; "Full card: the oldest log files are deleted until 10% is
 * free"; "card state [...] go[es] in the DAQ status bits". ADR 0021: "No
 * card state is ever a fault".
 */
#include <string.h>

#include "can_daq.h"
#include "data_log_card.h"
#include "unity.h"

static data_log_card_t c;

void setUp(void)
{
    data_log_card_init(&c);
}

void tearDown(void) {}

/* ---- states, Events and retries --------------------------------------- */

static void test_before_the_end_of_startup_the_card_is_waiting_and_not_tried(void)
{
    TEST_ASSERT_EQUAL(DATA_LOG_WAITING, c.state);
    TEST_ASSERT_FALSE(data_log_card_try_due(&c, 0u));
    TEST_ASSERT_FALSE(data_log_card_try_due(&c, 100000u));
}

static void test_the_first_mount_is_tried_at_once_when_startup_ends(void)
{
    data_log_card_start(&c);
    TEST_ASSERT_TRUE(data_log_card_try_due(&c, 1234u));
}

static void test_a_missing_card_writes_one_event_and_is_retried_every_5_s(void)
{
    data_log_card_start(&c);
    TEST_ASSERT_TRUE(data_log_card_mount_failed(&c, 1000u));
    TEST_ASSERT_EQUAL(DATA_LOG_MISSING, c.state);

    TEST_ASSERT_FALSE(data_log_card_try_due(&c, 5999u));
    TEST_ASSERT_TRUE(data_log_card_try_due(&c, 6000u));
    /* Still missing: no second Event. */
    TEST_ASSERT_FALSE(data_log_card_mount_failed(&c, 6000u));
    TEST_ASSERT_FALSE(data_log_card_try_due(&c, 10999u));
    TEST_ASSERT_TRUE(data_log_card_try_due(&c, 11000u));
}

static void test_a_card_that_mounts_logs_and_is_not_retried(void)
{
    data_log_card_start(&c);
    data_log_card_opened(&c, 500u);
    TEST_ASSERT_EQUAL(DATA_LOG_LOGGING, c.state);
    TEST_ASSERT_FALSE(data_log_card_try_due(&c, 100000u));
}

static void test_a_write_error_while_logging_writes_one_event_then_retries_every_5_s(void)
{
    data_log_card_start(&c);
    data_log_card_opened(&c, 0u);
    TEST_ASSERT_TRUE(data_log_card_write_failed(&c, 20000u));
    TEST_ASSERT_EQUAL(DATA_LOG_ERROR, c.state);
    TEST_ASSERT_FALSE(data_log_card_try_due(&c, 24999u));
    TEST_ASSERT_TRUE(data_log_card_try_due(&c, 25000u));

    /* The card was pulled: the retry finds none. Same incident, no Event. */
    TEST_ASSERT_FALSE(data_log_card_mount_failed(&c, 25000u));
    TEST_ASSERT_EQUAL(DATA_LOG_MISSING, c.state);
}

static void test_a_card_that_comes_back_and_fails_again_writes_a_new_event(void)
{
    data_log_card_start(&c);
    TEST_ASSERT_TRUE(data_log_card_mount_failed(&c, 0u));
    data_log_card_opened(&c, 5000u);
    TEST_ASSERT_TRUE(data_log_card_write_failed(&c, 6000u));
}

static void test_retries_keep_their_5_s_across_the_millisecond_wrap(void)
{
    data_log_card_start(&c);
    (void)data_log_card_mount_failed(&c, 0xFFFFF000u);
    TEST_ASSERT_FALSE(data_log_card_try_due(&c, 0xFFFFF000u + 4999u));
    TEST_ASSERT_TRUE(data_log_card_try_due(&c, 0xFFFFF000u + 5000u));
}

/* ---- roll and sync ---------------------------------------------------- */

static void test_a_file_is_rolled_after_10_minutes(void)
{
    data_log_card_start(&c);
    data_log_card_opened(&c, 0xFFFFFF00u);
    TEST_ASSERT_FALSE(data_log_card_roll_due(&c, 0xFFFFFF00u + 599999u));
    TEST_ASSERT_TRUE(data_log_card_roll_due(&c, 0xFFFFFF00u + 600000u));

    data_log_card_opened(&c, 0xFFFFFF00u + 600000u);
    TEST_ASSERT_FALSE(data_log_card_roll_due(&c, 0xFFFFFF00u + 600001u));
}

static void test_an_open_file_is_synced_every_second(void)
{
    data_log_card_start(&c);
    data_log_card_opened(&c, 100u);
    TEST_ASSERT_FALSE(data_log_card_sync_due(&c, 1099u));
    TEST_ASSERT_TRUE(data_log_card_sync_due(&c, 1100u));
    data_log_card_synced(&c, 1100u);
    TEST_ASSERT_FALSE(data_log_card_sync_due(&c, 2099u));
    TEST_ASSERT_TRUE(data_log_card_sync_due(&c, 2100u));
}

static void test_nothing_is_rolled_or_synced_without_an_open_file(void)
{
    data_log_card_start(&c);
    (void)data_log_card_mount_failed(&c, 0u);
    TEST_ASSERT_FALSE(data_log_card_roll_due(&c, 10000000u));
    TEST_ASSERT_FALSE(data_log_card_sync_due(&c, 10000000u));
}

/* ---- the DAQ status --------------------------------------------------- */

static void test_each_state_has_its_card_state_on_the_daq_bus(void)
{
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_MISSING_CHOICE,
                            data_log_card_choice(DATA_LOG_WAITING));
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_MISSING_CHOICE,
                            data_log_card_choice(DATA_LOG_MISSING));
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_LOGGING_CHOICE,
                            data_log_card_choice(DATA_LOG_LOGGING));
    TEST_ASSERT_EQUAL_UINT8(CAN_DAQ_VCU_DAQ_STATUS_CARD_STATE_ERROR_CHOICE,
                            data_log_card_choice(DATA_LOG_ERROR));
}

/* ---- names and space --------------------------------------------------- */

static void test_each_boot_has_its_own_directory_named_by_its_boot_number(void)
{
    char name[DATA_LOG_NAME_MAX];
    uint16_t boot = 0u;

    data_log_dir_name(42u, name);
    TEST_ASSERT_EQUAL_STRING("B00042", name);
    data_log_dir_name(65535u, name);
    TEST_ASSERT_EQUAL_STRING("B65535", name);

    TEST_ASSERT_TRUE(data_log_parse_dir("B00042", &boot));
    TEST_ASSERT_EQUAL_UINT16(42u, boot);
    TEST_ASSERT_FALSE(data_log_parse_dir("B0004", &boot));
    TEST_ASSERT_FALSE(data_log_parse_dir("B70000", &boot));
    TEST_ASSERT_FALSE(data_log_parse_dir("X00042", &boot));
    TEST_ASSERT_FALSE(data_log_parse_dir("LOG001.CSV", &boot));
}

static void test_files_are_numbered_in_their_boot_directory(void)
{
    char name[DATA_LOG_NAME_MAX];
    uint16_t file = 0u;

    data_log_file_name(3u, name);
    TEST_ASSERT_EQUAL_STRING("L0003.TDL", name);

    TEST_ASSERT_TRUE(data_log_parse_file("L0003.TDL", &file));
    TEST_ASSERT_EQUAL_UINT16(3u, file);
    TEST_ASSERT_FALSE(data_log_parse_file("L0003.CSV", &file));
    TEST_ASSERT_FALSE(data_log_parse_file("L003.TDL", &file));
    TEST_ASSERT_FALSE(data_log_parse_file("NOTES.TXT", &file));
}

static void test_space_is_made_while_less_than_10_percent_is_free(void)
{
    TEST_ASSERT_TRUE(data_log_card_needs_space(99u, 1000u));
    TEST_ASSERT_FALSE(data_log_card_needs_space(100u, 1000u));
    /* No overflow on a large card. */
    TEST_ASSERT_TRUE(data_log_card_needs_space(0x19999999u, 0xFFFFFFFFu));
    TEST_ASSERT_FALSE(data_log_card_needs_space(0x1999999Au, 0xFFFFFFFFu));
}

static void test_the_oldest_boot_is_the_one_furthest_behind_this_one_across_the_wrap(void)
{
    /* This boot is 3: boot 65530 is older than boot 1. */
    TEST_ASSERT_TRUE(data_log_boot_older(65530u, 1u, 3u));
    TEST_ASSERT_FALSE(data_log_boot_older(1u, 65530u, 3u));
    TEST_ASSERT_TRUE(data_log_boot_older(10u, 20u, 30u));
    /* This boot itself is the newest of all. */
    TEST_ASSERT_TRUE(data_log_boot_older(29u, 30u, 30u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_before_the_end_of_startup_the_card_is_waiting_and_not_tried);
    RUN_TEST(test_the_first_mount_is_tried_at_once_when_startup_ends);
    RUN_TEST(test_a_missing_card_writes_one_event_and_is_retried_every_5_s);
    RUN_TEST(test_a_card_that_mounts_logs_and_is_not_retried);
    RUN_TEST(test_a_write_error_while_logging_writes_one_event_then_retries_every_5_s);
    RUN_TEST(test_a_card_that_comes_back_and_fails_again_writes_a_new_event);
    RUN_TEST(test_retries_keep_their_5_s_across_the_millisecond_wrap);
    RUN_TEST(test_a_file_is_rolled_after_10_minutes);
    RUN_TEST(test_an_open_file_is_synced_every_second);
    RUN_TEST(test_nothing_is_rolled_or_synced_without_an_open_file);
    RUN_TEST(test_each_state_has_its_card_state_on_the_daq_bus);
    RUN_TEST(test_each_boot_has_its_own_directory_named_by_its_boot_number);
    RUN_TEST(test_files_are_numbered_in_their_boot_directory);
    RUN_TEST(test_space_is_made_while_less_than_10_percent_is_free);
    RUN_TEST(test_the_oldest_boot_is_the_one_furthest_behind_this_one_across_the_wrap);
    return UNITY_END();
}
