/*
 * Host tests for E2E Profile 11 (App/common/logic/Inc/e2e_p11.h).
 *
 * The expected bytes are the protect examples in the AUTOSAR "E2E Protocol
 * Specification" (AUTOSAR_PRS_E2EProtocol, FO R1.5.1), section 6.9.9
 * "E2E Profile 11 Protocol Examples", Data ID mode "both": 8-byte frame, CRC
 * in byte 0, counter in the low half of byte 1, all signal bytes zero.
 * Table 6.46: counter 0 gives CC 00 00 00 00 00 00 00.
 * Table 6.47: counter 1 gives 91 01 00 00 00 00 00 00.
 * Table 6.44 prints the Data ID as "0x01234", which is not a 16-bit value;
 * the published results are the ones for 0x0123.
 */
#include "e2e_p11.h"
#include "unity.h"

#define DATA_ID 0x0123u

void setUp(void) {}
void tearDown(void) {}

static void test_protect_matches_the_autosar_example_for_counter_0(void)
{
    e2e_p11_tx_t tx = {0};
    uint8_t frame[8] = {0};
    const uint8_t expected[8] = {0xCC, 0x00, 0, 0, 0, 0, 0, 0};

    e2e_p11_protect(&tx, DATA_ID, frame, sizeof frame);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);
}

static void test_protect_matches_the_autosar_example_for_counter_1(void)
{
    e2e_p11_tx_t tx = {0};
    uint8_t first[8] = {0};
    uint8_t second[8] = {0};
    const uint8_t expected[8] = {0x91, 0x01, 0, 0, 0, 0, 0, 0};

    e2e_p11_protect(&tx, DATA_ID, first, sizeof first);
    e2e_p11_protect(&tx, DATA_ID, second, sizeof second);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, second, sizeof second);
}

static void test_protect_counter_goes_from_14_back_to_0(void)
{
    e2e_p11_tx_t tx = {0};
    uint8_t frame[8] = {0};
    const uint8_t expected[8] = {0xCC, 0x00, 0, 0, 0, 0, 0, 0};

    for (int i = 0; i < 15; i++) {
        uint8_t earlier[8] = {0};
        e2e_p11_protect(&tx, DATA_ID, earlier, sizeof earlier);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)i, earlier[1]);
    }
    e2e_p11_protect(&tx, DATA_ID, frame, sizeof frame);

    /* The sixteenth frame is the first one again: counter 15 is never sent. */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof frame);
}

/*
 * The upper half of byte 1 belongs to a signal. The expected bytes are the
 * AUTOSAR examples for Data ID mode "nibble" (section 6.9.9.1, Data ID
 * 0x0123: Table 6.48, counter 0, 2A 10 00...; Table 6.49, counter 1,
 * 77 11 00...). That mode is the same calculation as mode "both" with Data ID
 * 0x0023 and the value 1 already in the upper half of byte 1.
 */
static void test_protect_keeps_and_covers_the_upper_half_of_byte_1(void)
{
    e2e_p11_tx_t tx = {0};
    uint8_t first[8] = {0x00, 0x1F, 0, 0, 0, 0, 0, 0};
    uint8_t second[8] = {0x00, 0x1F, 0, 0, 0, 0, 0, 0};
    const uint8_t expected_first[8] = {0x2A, 0x10, 0, 0, 0, 0, 0, 0};
    const uint8_t expected_second[8] = {0x77, 0x11, 0, 0, 0, 0, 0, 0};

    e2e_p11_protect(&tx, 0x0023u, first, sizeof first);
    e2e_p11_protect(&tx, 0x0023u, second, sizeof second);

    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_first, first, sizeof first);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected_second, second, sizeof second);
}

/* The two AUTOSAR example frames, as a receiver sees them. */
static const uint8_t FRAME_COUNTER_0[8] = {0xCC, 0x00, 0, 0, 0, 0, 0, 0};
static const uint8_t FRAME_COUNTER_1[8] = {0x91, 0x01, 0, 0, 0, 0, 0, 0};

static void test_check_accepts_the_autosar_examples_in_order(void)
{
    e2e_p11_rx_t rx = {0};

    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));
    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_1, sizeof FRAME_COUNTER_1));
}

static void test_check_rejects_a_bad_crc(void)
{
    e2e_p11_rx_t rx = {0};
    uint8_t frame[8] = {0xCC, 0x00, 0, 0, 0, 0, 0, 0};

    frame[5] ^= 0x01u; /* one signal bit flipped on the wire */

    TEST_ASSERT_EQUAL(E2E_P11_BAD_CRC,
        e2e_p11_check(&rx, DATA_ID, frame, sizeof frame));
}

static void test_check_rejects_a_frame_sent_under_another_data_id(void)
{
    e2e_p11_rx_t rx = {0};

    TEST_ASSERT_EQUAL(E2E_P11_BAD_CRC,
        e2e_p11_check(&rx, 0x0124u, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));
}

static void test_check_reports_a_repeated_counter(void)
{
    e2e_p11_rx_t rx = {0};

    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));
    TEST_ASSERT_EQUAL(E2E_P11_REPEATED,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));
}

/* Builds the frame a sender would produce with the given counter. */
static void sent_frame(uint8_t counter, uint8_t frame[8])
{
    e2e_p11_tx_t tx = {0};

    for (uint8_t i = 0; i <= counter; i++) {
        for (int b = 0; b < 8; b++) {
            frame[b] = 0;
        }
        e2e_p11_protect(&tx, DATA_ID, frame, 8);
    }
}

static void test_check_reports_a_skipped_counter_then_carries_on(void)
{
    e2e_p11_rx_t rx = {0};
    uint8_t frame[8];

    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));

    sent_frame(2, frame); /* counter 1 was lost */
    TEST_ASSERT_EQUAL(E2E_P11_SKIPPED,
        e2e_p11_check(&rx, DATA_ID, frame, sizeof frame));

    sent_frame(3, frame);
    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, frame, sizeof frame));
}

static void test_check_reports_a_counter_going_backwards_as_skipped(void)
{
    e2e_p11_rx_t rx = {0};

    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_1, sizeof FRAME_COUNTER_1));
    TEST_ASSERT_EQUAL(E2E_P11_SKIPPED,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));
}

static void test_check_accepts_the_counter_going_from_14_back_to_0(void)
{
    e2e_p11_rx_t rx = {0};
    uint8_t frame[8];

    sent_frame(14, frame);
    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, frame, sizeof frame));
    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));
}

static void test_check_ignores_the_counter_of_a_frame_with_a_bad_crc(void)
{
    e2e_p11_rx_t rx = {0};
    uint8_t corrupt[8];

    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));

    sent_frame(5, corrupt);
    corrupt[0] ^= 0xFFu;
    TEST_ASSERT_EQUAL(E2E_P11_BAD_CRC,
        e2e_p11_check(&rx, DATA_ID, corrupt, sizeof corrupt));

    /* Counter 1 is still the one expected next. */
    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_1, sizeof FRAME_COUNTER_1));
}

/*
 * Counter 15 is never sent. The frame here is the AUTOSAR counter-0 example
 * with the counter forced to 15 and the CRC that goes with it (0x90, from an
 * independent bitwise CRC-8 0x1D calculation over 23 01 0F 00 00 00 00 00 00).
 */
static void test_check_reports_counter_15_as_skipped_and_does_not_keep_it(void)
{
    e2e_p11_rx_t rx = {0};
    const uint8_t frame[8] = {0x90, 0x0F, 0, 0, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_0, sizeof FRAME_COUNTER_0));
    TEST_ASSERT_EQUAL(E2E_P11_SKIPPED,
        e2e_p11_check(&rx, DATA_ID, frame, sizeof frame));
    TEST_ASSERT_EQUAL(E2E_P11_OK,
        e2e_p11_check(&rx, DATA_ID, FRAME_COUNTER_1, sizeof FRAME_COUNTER_1));
}

/* A received frame can have any length: one too short to hold the CRC and
 * counter is rejected without reading past its end (ASan checks that). */
static void test_check_rejects_a_frame_too_short_to_carry_the_check(void)
{
    e2e_p11_rx_t rx = {0};
    const uint8_t one_byte[1] = {0x00};

    TEST_ASSERT_EQUAL(E2E_P11_BAD_CRC,
        e2e_p11_check(&rx, DATA_ID, one_byte, sizeof one_byte));
    TEST_ASSERT_EQUAL(E2E_P11_BAD_CRC,
        e2e_p11_check(&rx, DATA_ID, one_byte, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_protect_matches_the_autosar_example_for_counter_0);
    RUN_TEST(test_protect_matches_the_autosar_example_for_counter_1);
    RUN_TEST(test_protect_counter_goes_from_14_back_to_0);
    RUN_TEST(test_protect_keeps_and_covers_the_upper_half_of_byte_1);
    RUN_TEST(test_check_accepts_the_autosar_examples_in_order);
    RUN_TEST(test_check_rejects_a_bad_crc);
    RUN_TEST(test_check_rejects_a_frame_sent_under_another_data_id);
    RUN_TEST(test_check_reports_a_repeated_counter);
    RUN_TEST(test_check_reports_a_skipped_counter_then_carries_on);
    RUN_TEST(test_check_reports_a_counter_going_backwards_as_skipped);
    RUN_TEST(test_check_accepts_the_counter_going_from_14_back_to_0);
    RUN_TEST(test_check_ignores_the_counter_of_a_frame_with_a_bad_crc);
    RUN_TEST(test_check_reports_counter_15_as_skipped_and_does_not_keep_it);
    RUN_TEST(test_check_rejects_a_frame_too_short_to_carry_the_check);
    return UNITY_END();
}
