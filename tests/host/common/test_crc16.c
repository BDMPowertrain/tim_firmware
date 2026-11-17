/*
 * Host tests for CRC-16 (App/common/logic/Inc/crc16.h), against the
 * published check value of CRC-16/IBM-3740 (also called CCITT-FALSE).
 */
#include <string.h>

#include "crc16.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char CHECK[] = "123456789";

static void test_check_value(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x29B1u, crc16(CHECK, strlen(CHECK)));
}

static void test_nothing_is_the_initial_value(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, crc16(CHECK, 0u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_nothing_is_the_initial_value);
    return UNITY_END();
}
