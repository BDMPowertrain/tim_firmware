/*
 * Host tests for the Data log ring (App/common/logic/Inc/record_ring.h).
 *
 * ADR 0021: "At the end of each step the control step copies one record
 * into a fixed ring that only it writes and only the SD logger task reads.
 * [...] If the ring is full the new record is dropped and counted, and the
 * step never waits." Issue 53: "a full ring drops the new Record and counts
 * it"; "A gap in the record sequence numbers is a visible loss".
 */
#include <string.h>

#include "record_ring.h"
#include "unity.h"

#define DEPTH 4u
#define PAYLOAD_MAX 8u

static uint8_t storage[RECORD_RING_STORAGE_SIZE(DEPTH, PAYLOAD_MAX)];
static record_ring_t ring;

void setUp(void)
{
    record_ring_init(&ring, storage, sizeof storage, PAYLOAD_MAX);
}

void tearDown(void) {}

static bool push_byte(uint8_t value, uint32_t uptime_ms)
{
    return record_ring_push(&ring, 7u, uptime_ms, &value, 1u);
}

static void test_the_ring_holds_as_many_records_as_its_storage_has_slots(void)
{
    TEST_ASSERT_EQUAL_UINT32(DEPTH, record_ring_depth(&ring));
}

static void test_a_depth_that_is_not_a_power_of_two_is_rounded_down_to_one(void)
{
    /* So a slot's place stays continuous when the free-running indices
       wrap at 2^32. */
    static uint8_t odd[RECORD_RING_STORAGE_SIZE(7u, PAYLOAD_MAX)];
    record_ring_t r;

    record_ring_init(&r, odd, sizeof odd, PAYLOAD_MAX);
    TEST_ASSERT_EQUAL_UINT32(4u, record_ring_depth(&r));
}

static void test_an_empty_ring_gives_nothing(void)
{
    record_head_t head;
    uint8_t payload[PAYLOAD_MAX];

    TEST_ASSERT_FALSE(record_ring_pop(&ring, &head, payload, sizeof payload));
}

static void test_a_record_comes_out_as_it_went_in(void)
{
    const uint8_t in[3] = {0xA1u, 0xB2u, 0xC3u};
    record_head_t head;
    uint8_t out[PAYLOAD_MAX] = {0};

    TEST_ASSERT_TRUE(record_ring_push(&ring, 2u, 123456u, in, sizeof in));
    TEST_ASSERT_TRUE(record_ring_pop(&ring, &head, out, sizeof out));

    TEST_ASSERT_EQUAL_UINT8(2u, head.type);
    TEST_ASSERT_EQUAL_UINT16(3u, head.len);
    TEST_ASSERT_EQUAL_UINT32(123456u, head.uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(0u, head.seq);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, sizeof in);
}

static void test_records_come_out_oldest_first_with_rising_sequence_numbers(void)
{
    record_head_t head;
    uint8_t out[PAYLOAD_MAX];

    for (uint8_t i = 0; i < 3u; i++) {
        TEST_ASSERT_TRUE(push_byte((uint8_t)(10u + i), 100u + i));
    }
    for (uint8_t i = 0; i < 3u; i++) {
        TEST_ASSERT_TRUE(record_ring_pop(&ring, &head, out, sizeof out));
        TEST_ASSERT_EQUAL_UINT32(i, head.seq);
        TEST_ASSERT_EQUAL_UINT32(100u + i, head.uptime_ms);
        TEST_ASSERT_EQUAL_UINT8(10u + i, out[0]);
    }
    TEST_ASSERT_FALSE(record_ring_pop(&ring, &head, out, sizeof out));
}

static void test_a_full_ring_drops_the_new_record_counts_it_and_leaves_a_gap(void)
{
    record_head_t head;
    uint8_t out[PAYLOAD_MAX];

    for (uint8_t i = 0; i < DEPTH; i++) {
        TEST_ASSERT_TRUE(push_byte(i, i));
    }
    /* Two more: no room, both dropped, the ones already in kept. */
    TEST_ASSERT_FALSE(push_byte(0xEEu, 50u));
    TEST_ASSERT_FALSE(push_byte(0xEFu, 51u));
    TEST_ASSERT_EQUAL_UINT32(2u, record_ring_dropped(&ring));

    for (uint8_t i = 0; i < DEPTH; i++) {
        TEST_ASSERT_TRUE(record_ring_pop(&ring, &head, out, sizeof out));
        TEST_ASSERT_EQUAL_UINT8(i, out[0]);
    }

    /* The next record has room, and its sequence number shows the two lost. */
    TEST_ASSERT_TRUE(push_byte(0x55u, 60u));
    TEST_ASSERT_TRUE(record_ring_pop(&ring, &head, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT32(DEPTH + 2u, head.seq);
    TEST_ASSERT_EQUAL_UINT8(0x55u, out[0]);
}

static void test_the_ring_reuses_its_slots_for_ever(void)
{
    record_head_t head;
    uint8_t out[PAYLOAD_MAX];

    for (uint32_t i = 0; i < 1000u; i++) {
        TEST_ASSERT_TRUE(push_byte((uint8_t)i, i));
        TEST_ASSERT_TRUE(push_byte((uint8_t)(i + 1u), i));
        TEST_ASSERT_TRUE(record_ring_pop(&ring, &head, out, sizeof out));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)i, out[0]);
        TEST_ASSERT_TRUE(record_ring_pop(&ring, &head, out, sizeof out));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1u), out[0]);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, record_ring_dropped(&ring));
    TEST_ASSERT_EQUAL_UINT32(1999u, head.seq);
}

static void test_a_payload_longer_than_a_slot_is_dropped_and_counted(void)
{
    uint8_t big[PAYLOAD_MAX + 1u];
    record_head_t head;
    uint8_t out[PAYLOAD_MAX + 1u];

    memset(big, 0x11, sizeof big);
    TEST_ASSERT_FALSE(record_ring_push(&ring, 1u, 0u, big, sizeof big));
    TEST_ASSERT_EQUAL_UINT32(1u, record_ring_dropped(&ring));
    TEST_ASSERT_FALSE(record_ring_pop(&ring, &head, out, sizeof out));
}

static void test_a_reader_buffer_too_small_takes_the_record_but_none_of_its_payload(void)
{
    const uint8_t in[4] = {1u, 2u, 3u, 4u};
    record_head_t head;
    uint8_t out[2];

    TEST_ASSERT_TRUE(record_ring_push(&ring, 1u, 0u, in, sizeof in));
    TEST_ASSERT_TRUE(record_ring_pop(&ring, &head, out, sizeof out));
    TEST_ASSERT_EQUAL_UINT16(0u, head.len);
    TEST_ASSERT_FALSE(record_ring_pop(&ring, &head, out, sizeof out));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_ring_holds_as_many_records_as_its_storage_has_slots);
    RUN_TEST(test_a_depth_that_is_not_a_power_of_two_is_rounded_down_to_one);
    RUN_TEST(test_an_empty_ring_gives_nothing);
    RUN_TEST(test_a_record_comes_out_as_it_went_in);
    RUN_TEST(test_records_come_out_oldest_first_with_rising_sequence_numbers);
    RUN_TEST(test_a_full_ring_drops_the_new_record_counts_it_and_leaves_a_gap);
    RUN_TEST(test_the_ring_reuses_its_slots_for_ever);
    RUN_TEST(test_a_payload_longer_than_a_slot_is_dropped_and_counted);
    RUN_TEST(test_a_reader_buffer_too_small_takes_the_record_but_none_of_its_payload);
    return UNITY_END();
}
