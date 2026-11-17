/*
 * Host tests for the Data log's write batch (App/common/logic/Inc/log_batch.h).
 *
 * Issue 53: "The logger batches into a 4 KB buffer, writes whole sectors and
 * syncs every 1 s."
 */
#include <string.h>

#include "log_batch.h"
#include "unity.h"

static log_batch_t b;

void setUp(void)
{
    log_batch_init(&b);
}

void tearDown(void) {}

/* Bytes counting up from start, so order is checkable. */
static void fill(uint8_t *out, size_t n, uint32_t start)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)(start + i);
    }
}

static void test_an_empty_batch_has_no_sector_to_write(void)
{
    TEST_ASSERT_EQUAL_size_t(0u, log_batch_used(&b));
    TEST_ASSERT_EQUAL_size_t(0u, log_batch_sectors(&b));
    TEST_ASSERT_FALSE(log_batch_full(&b));
}

static void test_the_batch_is_4_kb_of_512_byte_sectors(void)
{
    TEST_ASSERT_EQUAL_size_t(4096u, LOG_BATCH_SIZE);
    TEST_ASSERT_EQUAL_size_t(512u, LOG_BATCH_SECTOR);
}

static void test_only_whole_sectors_are_offered_for_writing(void)
{
    uint8_t in[1300];

    fill(in, sizeof in, 0u);
    TEST_ASSERT_EQUAL_size_t(511u, log_batch_put(&b, in, 511u));
    TEST_ASSERT_EQUAL_size_t(0u, log_batch_sectors(&b));
    TEST_ASSERT_EQUAL_size_t(sizeof in - 511u, log_batch_put(&b, &in[511], sizeof in - 511u));
    TEST_ASSERT_EQUAL_size_t(1024u, log_batch_sectors(&b));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, log_batch_data(&b), 1024u);
}

static void test_a_put_takes_what_fits_and_the_batch_is_then_full(void)
{
    static uint8_t in[5000];

    fill(in, sizeof in, 7u);
    TEST_ASSERT_EQUAL_size_t(4096u, log_batch_put(&b, in, sizeof in));
    TEST_ASSERT_TRUE(log_batch_full(&b));
    TEST_ASSERT_EQUAL_size_t(4096u, log_batch_sectors(&b));
    TEST_ASSERT_EQUAL_size_t(0u, log_batch_put(&b, &in[4096], 1u));
}

static void test_written_bytes_leave_and_the_rest_moves_to_the_front_in_order(void)
{
    uint8_t in[1300];

    fill(in, sizeof in, 0u);
    (void)log_batch_put(&b, in, sizeof in);
    log_batch_consume(&b, log_batch_sectors(&b));

    TEST_ASSERT_EQUAL_size_t(1300u - 1024u, log_batch_used(&b));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&in[1024], log_batch_data(&b), 1300u - 1024u);

    /* More goes in after the rest. */
    uint8_t more[4];
    fill(more, sizeof more, 0x40u);
    TEST_ASSERT_EQUAL_size_t(4u, log_batch_put(&b, more, sizeof more));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(more, &log_batch_data(&b)[1300u - 1024u], sizeof more);
}

static void test_consuming_everything_empties_the_batch(void)
{
    uint8_t in[100];

    fill(in, sizeof in, 0u);
    (void)log_batch_put(&b, in, sizeof in);
    log_batch_consume(&b, log_batch_used(&b));
    TEST_ASSERT_EQUAL_size_t(0u, log_batch_used(&b));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_empty_batch_has_no_sector_to_write);
    RUN_TEST(test_the_batch_is_4_kb_of_512_byte_sectors);
    RUN_TEST(test_only_whole_sectors_are_offered_for_writing);
    RUN_TEST(test_a_put_takes_what_fits_and_the_batch_is_then_full);
    RUN_TEST(test_written_bytes_leave_and_the_rest_moves_to_the_front_in_order);
    RUN_TEST(test_consuming_everything_empties_the_batch);
    return UNITY_END();
}
