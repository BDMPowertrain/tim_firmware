/*
 * Host tests for the DAQ bus send queue (App/common/logic/Inc/frame_queue.h).
 *
 * Spec (issue 46): "its queue drops the oldest frame when full and never
 * makes a caller wait". Issue 36: "DAQ bus isolation: its own send queue
 * that drops the oldest frame when full".
 */
#include "can_frame.h"
#include "frame_queue.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static can_frame_t frame_with_id(uint32_t id)
{
    can_frame_t f = {0};
    f.id = id;
    f.len = 1u;
    f.data[0] = (uint8_t)id;
    return f;
}

static void test_an_empty_queue_gives_nothing(void)
{
    frame_queue_t q = {0};
    can_frame_t out;

    TEST_ASSERT_FALSE(frame_queue_pop(&q, &out));
}

static void test_frames_come_out_in_the_order_they_went_in(void)
{
    frame_queue_t q = {0};
    can_frame_t out;

    for (uint32_t id = 1u; id <= 3u; id++) {
        const can_frame_t f = frame_with_id(id);
        frame_queue_push(&q, &f);
    }

    for (uint32_t id = 1u; id <= 3u; id++) {
        TEST_ASSERT_TRUE(frame_queue_pop(&q, &out));
        TEST_ASSERT_EQUAL_UINT32(id, out.id);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)id, out.data[0]);
    }
    TEST_ASSERT_FALSE(frame_queue_pop(&q, &out));
    TEST_ASSERT_EQUAL_UINT32(0u, q.dropped);
}

static void test_a_full_queue_drops_its_oldest_frame_and_counts_it(void)
{
    frame_queue_t q = {0};
    can_frame_t out;

    /* Two more than fit: frames 1 and 2 are the oldest and are dropped. */
    for (uint32_t id = 1u; id <= FRAME_QUEUE_DEPTH + 2u; id++) {
        const can_frame_t f = frame_with_id(id);
        frame_queue_push(&q, &f);
    }

    TEST_ASSERT_EQUAL_UINT32(2u, q.dropped);
    for (uint32_t id = 3u; id <= FRAME_QUEUE_DEPTH + 2u; id++) {
        TEST_ASSERT_TRUE(frame_queue_pop(&q, &out));
        TEST_ASSERT_EQUAL_UINT32(id, out.id);
    }
    TEST_ASSERT_FALSE(frame_queue_pop(&q, &out));
}

static void test_the_queue_keeps_its_order_across_many_wraps(void)
{
    frame_queue_t q = {0};
    can_frame_t out;
    uint32_t next_in = 1u;
    uint32_t last_out = 0u;

    /* Push three, pop two, for longer than many laps of the ring. Drops
     * only ever remove the oldest, so what comes out always rises. */
    for (int round = 0; round < 100; round++) {
        for (int i = 0; i < 3; i++) {
            const can_frame_t f = frame_with_id(next_in++);
            frame_queue_push(&q, &f);
        }
        for (int i = 0; i < 2; i++) {
            TEST_ASSERT_TRUE(frame_queue_pop(&q, &out));
            TEST_ASSERT_GREATER_THAN_UINT32(last_out, out.id);
            last_out = out.id;
        }
    }

    /* The ring first overflows in round 15 (14 frames held, 3 pushed), and
     * from then on every round drops one and ends holding 14: rounds 15 to
     * 100 drop 86. 300 in = 200 out + 86 dropped + 14 held, and the 14 held
     * are the newest, since the oldest go first. */
    TEST_ASSERT_EQUAL_UINT32(86u, q.dropped);
    for (uint32_t id = 287u; id <= 300u; id++) {
        TEST_ASSERT_TRUE(frame_queue_pop(&q, &out));
        TEST_ASSERT_EQUAL_UINT32(id, out.id);
    }
    TEST_ASSERT_FALSE(frame_queue_pop(&q, &out));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_empty_queue_gives_nothing);
    RUN_TEST(test_frames_come_out_in_the_order_they_went_in);
    RUN_TEST(test_a_full_queue_drops_its_oldest_frame_and_counts_it);
    RUN_TEST(test_the_queue_keeps_its_order_across_many_wraps);
    return UNITY_END();
}
