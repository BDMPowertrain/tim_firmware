/*
 * Host tests for the Event log (App/common/logic/Inc/event_log.h, ADR
 * 0014), through open, append and read on the RAM-backed flash adapter. A
 * "reset" is a fresh event_log_open() on the same flash.
 */
#include <string.h>

#include "event_log.h"
#include "ram_flash.h"
#include "unity.h"

static event_log_t log_;

void setUp(void)
{
    ram_flash_reset();
    event_log_open(&log_, ram_flash());
}

void tearDown(void) {}

/* A reset: power back, then the start-up open. */
static void reset(void)
{
    ram_flash_power_on();
    event_log_open(&log_, ram_flash());
}

/* Appends an Event whose code and data say which it is; must succeed. */
static void append(uint16_t code)
{
    uint8_t data[EVENT_DATA_SIZE];

    memset(data, (int)(code & 0xFFu), sizeof data);
    TEST_ASSERT_EQUAL(EVENT_APPEND_OK, event_log_append(&log_, code, 1000u + code, data));
}

static void append_n(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        append((uint16_t)i);
    }
}

/* Walks from the newest Event to the oldest, every one reading whole and,
   within a boot, each sequence number one less than the last; returns how
   many, and the oldest's sequence number. */
static uint32_t walk_all(uint32_t *oldest_seq)
{
    event_pos_t pos;
    event_t ev;
    uint32_t n = 0u;
    uint16_t boot = 0u;
    bool more = event_log_newest(&log_, &pos);

    while (more) {
        TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
        if (n > 0u && ev.boot == boot) {
            TEST_ASSERT_EQUAL_UINT32(*oldest_seq - 1u, ev.seq);
        }
        *oldest_seq = ev.seq;
        boot = ev.boot;
        n++;
        more = event_log_older(&log_, &pos);
    }
    return n;
}

/* Reads the newest Event; it must read whole. */
static event_t newest(void)
{
    event_pos_t pos;
    event_t ev;

    TEST_ASSERT_TRUE(event_log_newest(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
    return ev;
}

static void test_an_event_reads_back_whole(void)
{
    append(7u);

    event_t ev = newest();
    uint8_t want[EVENT_DATA_SIZE];

    memset(want, 7, sizeof want);
    TEST_ASSERT_EQUAL_UINT16(1u, ev.boot);
    TEST_ASSERT_EQUAL_UINT16(7u, ev.code);
    TEST_ASSERT_EQUAL_UINT32(1007u, ev.uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, ev.seq);
    TEST_ASSERT_EQUAL_MEMORY(want, ev.data, EVENT_DATA_SIZE);
}

static void test_a_reset_recovers_the_boot_number_and_carries_on(void)
{
    append(1u);
    append(2u);
    reset();
    TEST_ASSERT_EQUAL_UINT16(2u, log_.boot);
    append(3u);
    reset();
    TEST_ASSERT_EQUAL_UINT16(3u, log_.boot);
    append(4u);

    event_pos_t pos;
    event_t ev;

    TEST_ASSERT_TRUE(event_log_newest(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_EQUAL_UINT16(4u, ev.code);
    TEST_ASSERT_EQUAL_UINT16(3u, ev.boot);
    TEST_ASSERT_EQUAL_UINT32(0u, ev.seq);

    TEST_ASSERT_TRUE(event_log_older(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_EQUAL_UINT16(3u, ev.code);
    TEST_ASSERT_EQUAL_UINT16(2u, ev.boot);

    TEST_ASSERT_TRUE(event_log_older(&log_, &pos));
    TEST_ASSERT_TRUE(event_log_older(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_EQUAL_UINT16(1u, ev.code);
    TEST_ASSERT_EQUAL_UINT16(1u, ev.boot);
    TEST_ASSERT_EQUAL_UINT32(0u, ev.seq);
    TEST_ASSERT_FALSE(event_log_older(&log_, &pos));
}

static void test_a_reset_with_nothing_written_keeps_the_empty_log_at_boot_1(void)
{
    reset();
    TEST_ASSERT_EQUAL_UINT16(1u, log_.boot);

    event_pos_t pos;

    TEST_ASSERT_FALSE(event_log_newest(&log_, &pos));
}

static void test_the_ring_wraps_by_erasing_the_oldest_page(void)
{
    const uint32_t full = EVENT_LOG_PAGES * EVENT_LOG_SLOTS;
    uint32_t oldest = 0u;

    append_n(full);
    TEST_ASSERT_EQUAL_UINT32(full, walk_all(&oldest));
    TEST_ASSERT_EQUAL_UINT32(0u, oldest);

    append_n(10u); /* the first page goes */
    TEST_ASSERT_EQUAL_UINT32(full - EVENT_LOG_SLOTS + 10u, walk_all(&oldest));
    TEST_ASSERT_EQUAL_UINT32(EVENT_LOG_SLOTS, oldest);

    reset();
    TEST_ASSERT_EQUAL_UINT16(2u, log_.boot);
    TEST_ASSERT_EQUAL_UINT32(full - EVENT_LOG_SLOTS + 10u, walk_all(&oldest));
    TEST_ASSERT_EQUAL_UINT32(full + 10u - 1u, newest().seq);
}

static void test_the_oldest_walks_forward_to_the_newest(void)
{
    append_n(EVENT_LOG_SLOTS + 5u);

    event_pos_t pos;
    event_t ev;
    uint32_t n = 1u;

    TEST_ASSERT_TRUE(event_log_newest(&log_, &pos));
    while (event_log_older(&log_, &pos)) {
    }
    TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_EQUAL_UINT32(0u, ev.seq);
    while (event_log_newer(&log_, &pos)) {
        TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
        TEST_ASSERT_EQUAL_UINT32(n, ev.seq);
        n++;
    }
    TEST_ASSERT_EQUAL_UINT32(EVENT_LOG_SLOTS + 5u, n);
}

/* Where the slot-th Event of the first page sits. */
static uint32_t first_page_slot(uint32_t slot)
{
    return EVENT_LOG_ADDR + EVENT_SIZE + slot * EVENT_SIZE;
}

static void test_an_event_with_a_bad_crc_is_rejected_and_skipped(void)
{
    append_n(3u);
    reset();
    append_n(2u); /* boot 2 */
    ram_flash_flip(first_page_slot(4u) + 12u, 0x01u); /* the newest's data */
    ram_flash_flip(first_page_slot(3u), 0x80u);        /* the next one's boot number */
    reset();

    /* The newest whole Event is boot 1's, so this start is boot 2 again. */
    TEST_ASSERT_EQUAL_UINT16(2u, log_.boot);
    TEST_ASSERT_EQUAL_UINT32(0u, log_.repairs);

    event_pos_t pos;
    event_t ev;

    TEST_ASSERT_TRUE(event_log_newest(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_BAD_CRC, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_TRUE(event_log_older(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_BAD_CRC, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_TRUE(event_log_older(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_EQUAL_UINT32(2u, ev.seq);

    /* Appending goes on after the rejected ones. */
    append(9u);
    TEST_ASSERT_EQUAL_UINT16(9u, newest().code);
}

/* Writes `before` Events, then one more with the power cut after `ops`
   flash operations, then resets. */
static void cut_the_next_write(uint32_t before, uint32_t ops)
{
    ram_flash_reset();
    event_log_open(&log_, ram_flash());
    append_n(before);
    ram_flash_cut_after(ops);

    uint8_t data[EVENT_DATA_SIZE] = {0};

    TEST_ASSERT_NOT_EQUAL(EVENT_APPEND_OK, event_log_append(&log_, 0xEEEEu, 0u, data));
    reset();
}

/* After a cut write: every Event before it reads whole, this start is boot
   2, and the log takes and reads back new Events. */
static void assert_recovered(uint32_t before)
{
    uint32_t oldest = 0u;

    TEST_ASSERT_EQUAL_UINT16(2u, log_.boot);
    TEST_ASSERT_EQUAL_UINT32(before, walk_all(&oldest));
    TEST_ASSERT_EQUAL_UINT16(before - 1u, newest().code);
    append(0x77u);
    TEST_ASSERT_EQUAL_UINT16(0x77u, newest().code);
    TEST_ASSERT_EQUAL_UINT16(2u, newest().boot);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, walk_all(&oldest));
}

static void test_a_write_torn_at_any_word_inside_a_page_is_repaired(void)
{
    const uint32_t words = EVENT_SIZE / FLASH_IF_WORD_SIZE;

    for (uint32_t ops = 0; ops < words; ops++) {
        cut_the_next_write(5u, ops);

        TEST_ASSERT_EQUAL_UINT32(1u, log_.repairs);
        TEST_ASSERT_EQUAL_UINT8(1u, log_.repair_count);
        TEST_ASSERT_EQUAL_UINT8(0u, log_.repair[0].page);
        TEST_ASSERT_EQUAL_UINT8(5u, log_.repair[0].kept);
        TEST_ASSERT_FALSE(log_.repair[0].header);
        TEST_ASSERT_TRUE(log_.repair[0].erased);
        assert_recovered(5u);
    }
}

static void test_a_write_torn_at_any_word_while_opening_a_page_is_repaired(void)
{
    /* The erase of the next page, its header's double words, then the
       Event's. */
    const uint32_t header_ops = 1u + EVENT_SIZE / FLASH_IF_WORD_SIZE;
    const uint32_t all_ops = header_ops + EVENT_SIZE / FLASH_IF_WORD_SIZE;

    for (uint32_t ops = 0; ops < all_ops; ops++) {
        cut_the_next_write(EVENT_LOG_SLOTS, ops);

        TEST_ASSERT_EQUAL_UINT32(1u, log_.repairs);
        TEST_ASSERT_EQUAL_UINT8(1u, log_.repair[0].page);
        TEST_ASSERT_EQUAL_UINT8(0u, log_.repair[0].kept);
        TEST_ASSERT_EQUAL(ops < header_ops, log_.repair[0].header);
        assert_recovered(EVENT_LOG_SLOTS);
    }
}

static void test_a_clean_log_needs_no_repair(void)
{
    append_n(EVENT_LOG_SLOTS + 1u);
    reset();
    TEST_ASSERT_EQUAL_UINT32(0u, log_.repairs);
    TEST_ASSERT_EQUAL_UINT8(0u, log_.repair_count);
}

static void test_an_event_whose_read_faults_is_skipped_by_the_reader(void)
{
    append_n(3u);
    ram_flash_fail_read(first_page_slot(1u), 1u);

    event_pos_t pos;
    event_t ev;

    TEST_ASSERT_TRUE(event_log_newest(&log_, &pos));
    TEST_ASSERT_TRUE(event_log_older(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_FAULT, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_TRUE(event_log_older(&log_, &pos));
    TEST_ASSERT_EQUAL(EVENT_READ_OK, event_log_read(&log_, pos, &ev));
    TEST_ASSERT_EQUAL_UINT32(0u, ev.seq);
}

static void test_a_cut_while_erasing_the_oldest_page_loses_only_that_page(void)
{
    const uint32_t full = EVENT_LOG_PAGES * EVENT_LOG_SLOTS;
    uint32_t oldest = 0u;

    append_n(full);
    ram_flash_cut_after(0u); /* the erase of the first page */

    uint8_t data[EVENT_DATA_SIZE] = {0};

    TEST_ASSERT_EQUAL(EVENT_APPEND_ERASE_FAILED, event_log_append(&log_, 1u, 0u, data));
    reset();

    TEST_ASSERT_EQUAL_UINT32(1u, log_.repairs);
    TEST_ASSERT_TRUE(log_.repair[0].header);
    TEST_ASSERT_EQUAL_UINT16(2u, log_.boot);
    TEST_ASSERT_EQUAL_UINT32(full - EVENT_LOG_SLOTS, walk_all(&oldest));
    TEST_ASSERT_EQUAL_UINT32(EVENT_LOG_SLOTS, oldest);

    append(5u); /* opens the first page again, as the newest */
    TEST_ASSERT_EQUAL_UINT16(5u, newest().code);
    TEST_ASSERT_EQUAL_UINT32(full - EVENT_LOG_SLOTS + 1u, walk_all(&oldest));
}

static void test_without_flash_nothing_is_written_or_read(void)
{
    uint8_t data[EVENT_DATA_SIZE] = {0};
    event_pos_t pos;

    event_log_open(&log_, NULL);
    TEST_ASSERT_EQUAL_UINT16(1u, log_.boot);
    TEST_ASSERT_EQUAL(EVENT_APPEND_NO_FLASH, event_log_append(&log_, 1u, 0u, data));
    TEST_ASSERT_FALSE(event_log_newest(&log_, &pos));
    TEST_ASSERT_EQUAL_UINT32(0u, ram_flash_ops());
}

static void test_a_slot_not_yet_written_reads_blank(void)
{
    append_n(2u);

    const event_pos_t past = { .page = 0u, .slot = 2u };
    const event_pos_t other = { .page = 5u, .slot = 0u };
    event_t ev;

    TEST_ASSERT_EQUAL(EVENT_READ_BLANK, event_log_read(&log_, past, &ev));
    TEST_ASSERT_EQUAL(EVENT_READ_BLANK, event_log_read(&log_, other, &ev));
}

static void test_an_event_cut_between_two_double_words_is_repaired(void)
{
    const uint8_t word[FLASH_IF_WORD_SIZE] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    append_n(2u);
    /* Power lost after the third slot's first double word, before its
       second: readable, partly erased. */
    TEST_ASSERT_TRUE(ram_flash()->write(NULL, first_page_slot(2u), word));
    reset();

    TEST_ASSERT_EQUAL_UINT32(1u, log_.repairs);
    TEST_ASSERT_EQUAL_UINT8(2u, log_.repair[0].kept);
    TEST_ASSERT_FALSE(log_.repair[0].header);
    assert_recovered(2u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_event_reads_back_whole);
    RUN_TEST(test_a_reset_recovers_the_boot_number_and_carries_on);
    RUN_TEST(test_a_reset_with_nothing_written_keeps_the_empty_log_at_boot_1);
    RUN_TEST(test_the_ring_wraps_by_erasing_the_oldest_page);
    RUN_TEST(test_the_oldest_walks_forward_to_the_newest);
    RUN_TEST(test_an_event_with_a_bad_crc_is_rejected_and_skipped);
    RUN_TEST(test_a_write_torn_at_any_word_inside_a_page_is_repaired);
    RUN_TEST(test_a_write_torn_at_any_word_while_opening_a_page_is_repaired);
    RUN_TEST(test_a_clean_log_needs_no_repair);
    RUN_TEST(test_an_event_whose_read_faults_is_skipped_by_the_reader);
    RUN_TEST(test_a_cut_while_erasing_the_oldest_page_loses_only_that_page);
    RUN_TEST(test_without_flash_nothing_is_written_or_read);
    RUN_TEST(test_a_slot_not_yet_written_reads_blank);
    RUN_TEST(test_an_event_cut_between_two_double_words_is_repaired);
    return UNITY_END();
}
