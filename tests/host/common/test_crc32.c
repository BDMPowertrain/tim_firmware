/*
 * Host tests for CRC-32 (App/common/logic/Inc/crc32.h), against the
 * published check value of CRC-32/ISO-HDLC (the zlib and Ethernet CRC).
 */
#include <string.h>

#include "crc32.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char CHECK[] = "123456789";

static void test_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32_update(0u, CHECK, strlen(CHECK)));
}

static void test_nothing_is_zero(void)
{
    TEST_ASSERT_EQUAL_HEX32(0u, crc32_update(0u, CHECK, 0u));
}

static void test_in_pieces_equals_in_one(void)
{
    uint32_t crc = crc32_update(0u, CHECK, 4u);

    crc = crc32_update(crc, CHECK + 4, 5u);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_nothing_is_zero);
    RUN_TEST(test_in_pieces_equals_in_one);
    return UNITY_END();
}
