/*
 * Host tests for the cell-monitor chain's frames (App/bms/logic/Inc/bq_frame.h).
 *
 * Issue 62: "Frame building and response parsing for the chain are logic and
 * host-tested, including a bad CRC and a short response."
 *
 * The expected command frames are TI's own, copied byte for byte from the
 * BQ79600-Q1 Software Design Reference (SLUAA17A), sections 2.1 to 2.3.
 */
#include <string.h>

#include "bq_frame.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define ASSERT_FRAME(expected, actual, len)                              \
    do {                                                                 \
        TEST_ASSERT_EQUAL_UINT(sizeof(expected), (len));                 \
        TEST_ASSERT_EQUAL_HEX8_ARRAY((expected), (actual), sizeof(expected)); \
    } while (0)

static void test_a_single_write_to_the_bridge_matches_ti_wake_example(void)
{
    /* SLUAA17A 2.1 step 3: CONTROL1[SEND_WAKE] = 1 on the BQ79600. */
    static const uint8_t expected[] = {0x90, 0x00, 0x03, 0x09, 0x20, 0x13, 0x95};
    const uint8_t data[] = {0x20};
    uint8_t frame[BQ_FRAME_MAX];

    const size_t len = bq_frame_write(frame, BQ_SINGLE_WRITE, 0x00u, 0x0309u, data, 1u);

    ASSERT_FRAME(expected, frame, len);
}

static void test_stack_broadcast_and_single_writes_match_ti_addressing_example(void)
{
    /* SLUAA17A 2.2.2 steps 1, 2 and 5. */
    static const uint8_t dll[] = {0xB0, 0x03, 0x43, 0x00, 0xE7, 0xD4};
    static const uint8_t autoAddr[] = {0xD0, 0x03, 0x09, 0x01, 0x0F, 0x74};
    static const uint8_t top[] = {0x90, 0x03, 0x03, 0x08, 0x03, 0x53, 0x98};
    const uint8_t zero[] = {0x00};
    const uint8_t one[] = {0x01};
    const uint8_t three[] = {0x03};
    uint8_t frame[BQ_FRAME_MAX];

    ASSERT_FRAME(dll, frame, bq_frame_write(frame, BQ_STACK_WRITE, 0u, 0x0343u, zero, 1u));
    ASSERT_FRAME(autoAddr, frame,
                 bq_frame_write(frame, BQ_BROADCAST_WRITE, 0u, 0x0309u, one, 1u));
    ASSERT_FRAME(top, frame, bq_frame_write(frame, BQ_SINGLE_WRITE, 3u, 0x0308u, three, 1u));
}

static void test_a_four_byte_write_matches_ti_template(void)
{
    /* SLUAA17A Table 1-4: stack write of 4 bytes to 0x100. */
    static const uint8_t expected[] = {0xB3, 0x01, 0x00, 0x02, 0xB7, 0x78, 0xBC, 0x0A, 0x35};
    const uint8_t data[] = {0x02, 0xB7, 0x78, 0xBC};
    uint8_t frame[BQ_FRAME_MAX];

    ASSERT_FRAME(expected, frame,
                 bq_frame_write(frame, BQ_STACK_WRITE, 0u, 0x0100u, data, sizeof data));
}

static void test_stack_and_single_reads_match_ti_examples(void)
{
    /* SLUAA17A 2.2.2 step 6 and 2.3.2 step 4; Table 1-1. */
    static const uint8_t dummy[] = {0xA0, 0x03, 0x43, 0x00, 0xE3, 0x14};
    static const uint8_t cells[] = {0xA0, 0x05, 0x68, 0x1F, 0x5C, 0x2D};
    static const uint8_t single[] = {0x80, 0x01, 0x02, 0x15, 0x0B, 0xCA, 0xB5};
    uint8_t frame[BQ_FRAME_MAX];

    ASSERT_FRAME(dummy, frame, bq_frame_read(frame, BQ_STACK_READ, 0u, 0x0343u, 1u));
    ASSERT_FRAME(cells, frame, bq_frame_read(frame, BQ_STACK_READ, 0u, 0x0568u, 32u));
    ASSERT_FRAME(single, frame, bq_frame_read(frame, BQ_SINGLE_READ, 1u, 0x0215u, 12u));
}

static void test_a_write_of_no_bytes_or_more_than_eight_builds_nothing(void)
{
    const uint8_t data[9] = {0};
    uint8_t frame[BQ_FRAME_MAX];

    TEST_ASSERT_EQUAL_UINT(0u, bq_frame_write(frame, BQ_STACK_WRITE, 0u, 0x0318u, data, 0u));
    TEST_ASSERT_EQUAL_UINT(0u, bq_frame_write(frame, BQ_STACK_WRITE, 0u, 0x0318u, data, 9u));
    TEST_ASSERT_EQUAL_UINT(13u, bq_frame_write(frame, BQ_STACK_WRITE, 0u, 0x0318u, data, 8u));
}

static void test_a_read_of_no_bytes_or_more_than_128_builds_nothing(void)
{
    uint8_t frame[BQ_FRAME_MAX];

    TEST_ASSERT_EQUAL_UINT(0u, bq_frame_read(frame, BQ_STACK_READ, 0u, 0x0578u, 0u));
    TEST_ASSERT_EQUAL_UINT(0u, bq_frame_read(frame, BQ_STACK_READ, 0u, 0x0578u, 129u));
    TEST_ASSERT_EQUAL_UINT(6u, bq_frame_read(frame, BQ_STACK_READ, 0u, 0x0578u, 128u));
}

/* ---- responses --------------------------------------------------------- */

/* Appends one response frame from device dev, register reg, sealed with the
   CRC the chain would send (checked independently against TI's examples
   above through bq_frame_write). */
static size_t put_response(uint8_t *buf, uint8_t dev, uint16_t reg, const uint8_t *data,
                           size_t n)
{
    buf[0] = (uint8_t)(n - 1u);
    buf[1] = dev;
    buf[2] = (uint8_t)(reg >> 8);
    buf[3] = (uint8_t)reg;
    memcpy(&buf[4], data, n);
    const uint16_t crc = bq_crc(buf, 4u + n);
    buf[4u + n] = (uint8_t)crc;
    buf[5u + n] = (uint8_t)(crc >> 8);
    return 6u + n;
}

static void test_the_crc_over_a_whole_frame_is_zero(void)
{
    static const uint8_t ti[] = {0x90, 0x00, 0x03, 0x09, 0x20, 0x13, 0x95};

    TEST_ASSERT_EQUAL_HEX16(0x0000u, bq_crc(ti, sizeof ti));
}

static void test_two_response_frames_are_read_in_turn(void)
{
    uint8_t buf[64];
    const uint8_t a[] = {0x02};
    const uint8_t b[] = {0x11, 0x22, 0x33};
    size_t len = put_response(buf, 2u, 0x0306u, a, sizeof a);
    len += put_response(&buf[len], 1u, 0x0306u, b, sizeof b);

    size_t pos = 0u;
    bq_response_t r;

    TEST_ASSERT_EQUAL(BQ_RESPONSE_OK, bq_response_next(buf, len, &pos, &r));
    TEST_ASSERT_EQUAL_UINT8(2u, r.dev);
    TEST_ASSERT_EQUAL_HEX16(0x0306u, r.reg);
    TEST_ASSERT_EQUAL_UINT(1u, r.len);
    TEST_ASSERT_EQUAL_HEX8(0x02, r.data[0]);

    TEST_ASSERT_EQUAL(BQ_RESPONSE_OK, bq_response_next(buf, len, &pos, &r));
    TEST_ASSERT_EQUAL_UINT8(1u, r.dev);
    TEST_ASSERT_EQUAL_UINT(3u, r.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(b, r.data, 3);

    TEST_ASSERT_EQUAL(BQ_RESPONSE_SHORT, bq_response_next(buf, len, &pos, &r));
}

static void test_a_flipped_bit_is_a_bad_crc(void)
{
    uint8_t buf[32];
    const uint8_t d[] = {0x40, 0x00};
    const size_t len = put_response(buf, 5u, 0x0578u, d, sizeof d);
    buf[5] ^= 0x01u;

    size_t pos = 0u;
    bq_response_t r;

    TEST_ASSERT_EQUAL(BQ_RESPONSE_BAD_CRC, bq_response_next(buf, len, &pos, &r));
}

static void test_a_frame_cut_short_is_short(void)
{
    uint8_t buf[32];
    const uint8_t d[] = {0x40, 0x00, 0x12};
    const size_t len = put_response(buf, 5u, 0x0578u, d, sizeof d);

    size_t pos = 0u;
    bq_response_t r;

    TEST_ASSERT_EQUAL(BQ_RESPONSE_SHORT, bq_response_next(buf, len - 1u, &pos, &r));
    pos = 0u;
    TEST_ASSERT_EQUAL(BQ_RESPONSE_SHORT, bq_response_next(buf, 3u, &pos, &r));
}

static void test_idle_bytes_where_a_frame_should_start_are_not_a_response(void)
{
    /* The bridge hands out 0xFF for a byte it does not hold (BQ79600-Q1
       data sheet 7.3.2.1.2.2.1): bit 7 set says command, not response. */
    uint8_t buf[16];
    memset(buf, 0xFF, sizeof buf);

    size_t pos = 0u;
    bq_response_t r;

    TEST_ASSERT_EQUAL(BQ_RESPONSE_NOT_A_RESPONSE, bq_response_next(buf, sizeof buf, &pos, &r));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_single_write_to_the_bridge_matches_ti_wake_example);
    RUN_TEST(test_stack_broadcast_and_single_writes_match_ti_addressing_example);
    RUN_TEST(test_a_four_byte_write_matches_ti_template);
    RUN_TEST(test_stack_and_single_reads_match_ti_examples);
    RUN_TEST(test_a_write_of_no_bytes_or_more_than_eight_builds_nothing);
    RUN_TEST(test_a_read_of_no_bytes_or_more_than_128_builds_nothing);
    RUN_TEST(test_the_crc_over_a_whole_frame_is_zero);
    RUN_TEST(test_two_response_frames_are_read_in_turn);
    RUN_TEST(test_a_flipped_bit_is_a_bad_crc);
    RUN_TEST(test_a_frame_cut_short_is_short);
    RUN_TEST(test_idle_bytes_where_a_frame_should_start_are_not_a_response);
    return UNITY_END();
}
