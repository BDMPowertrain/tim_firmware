/* SD card SPI-mode protocol pieces (sd_proto.h). The expected command
   frames are the SD Physical Layer Simplified Specification's own examples
   and the values every SPI-mode driver sends (CMD0 ends 0x95, CMD8 with
   0x1AA ends 0x87), not a CRC recomputed here. */
#include "unity.h"

#include "sd_proto.h"

void setUp(void) {}
void tearDown(void) {}

static void assert_frame(const uint8_t expected[SD_PROTO_COMMAND_SIZE],
                         uint8_t index, uint32_t arg)
{
    uint8_t frame[SD_PROTO_COMMAND_SIZE];

    sd_proto_command(index, arg, frame);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, SD_PROTO_COMMAND_SIZE);
}

static void test_cmd0_frame_carries_the_crc_the_card_checks(void)
{
    static const uint8_t expected[] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };
    assert_frame(expected, 0u, 0u);
}

static void test_cmd8_frame_carries_the_crc_the_card_checks(void)
{
    static const uint8_t expected[] = { 0x48, 0x00, 0x00, 0x01, 0xAA, 0x87 };
    assert_frame(expected, 8u, 0x1AAu);
}

/* Commands whose CRC the card ignores while CRC checking is off still get
   the right one, so turning it on (CMD59) would change nothing here. */
static void test_other_commands_carry_their_crc(void)
{
    static const uint8_t cmd17[] = { 0x51, 0x00, 0x00, 0x00, 0x00, 0x55 };
    static const uint8_t cmd55[] = { 0x77, 0x00, 0x00, 0x00, 0x00, 0x65 };
    static const uint8_t acmd41[] = { 0x69, 0x40, 0x00, 0x00, 0x00, 0x77 };
    static const uint8_t cmd58[] = { 0x7A, 0x00, 0x00, 0x00, 0x00, 0xFD };

    assert_frame(cmd17, 17u, 0u);
    assert_frame(cmd55, 55u, 0u);
    assert_frame(acmd41, 41u, 0x40000000u);
    assert_frame(cmd58, 58u, 0u);
}

/* R7's last four bytes after CMD8 0x1AA: the voltage it accepts (0x1 =
   2.7-3.6 V) and the check pattern echoed back. */
static void test_r7_accepts_only_our_voltage_and_echoed_pattern(void)
{
    static const uint8_t ok[] = { 0x00, 0x00, 0x01, 0xAA };
    static const uint8_t no_voltage[] = { 0x00, 0x00, 0x00, 0xAA };
    static const uint8_t wrong_echo[] = { 0x00, 0x00, 0x01, 0x55 };

    TEST_ASSERT_TRUE(sd_proto_r7_ok(ok));
    TEST_ASSERT_FALSE(sd_proto_r7_ok(no_voltage));
    TEST_ASSERT_FALSE(sd_proto_r7_ok(wrong_echo));
}

/* Data response token, section 7.3.3.1: xxx0sss1, sss = 010 accepted,
   101 CRC error, 110 write error. The top three bits are undefined. */
static void test_data_response_accepts_only_status_010(void)
{
    TEST_ASSERT_TRUE(sd_proto_data_accepted(0x05u));
    TEST_ASSERT_TRUE(sd_proto_data_accepted(0xE5u));
    TEST_ASSERT_FALSE(sd_proto_data_accepted(0x0Bu));
    TEST_ASSERT_FALSE(sd_proto_data_accepted(0x0Du));
    TEST_ASSERT_FALSE(sd_proto_data_accepted(0xFFu)); /* no card on MISO */
    TEST_ASSERT_FALSE(sd_proto_data_accepted(0x00u)); /* MISO stuck low */
}

/* OCR bit 30 (CCS), the top byte's 0x40: set on SDHC and SDXC, which
   take a block number; clear on SDSC, which takes a byte offset. */
static void test_ocr_ccs_picks_block_or_byte_address(void)
{
    static const uint8_t sdhc[] = { 0xC0, 0xFF, 0x80, 0x00 };
    static const uint8_t sdsc[] = { 0x80, 0xFF, 0x80, 0x00 };

    TEST_ASSERT_TRUE(sd_proto_ocr_block_addressed(sdhc));
    TEST_ASSERT_FALSE(sd_proto_ocr_block_addressed(sdsc));
}

static void test_sector_address_is_block_number_or_byte_offset(void)
{
    TEST_ASSERT_EQUAL_UINT32(1000u, sd_proto_address(true, 1000u));
    TEST_ASSERT_EQUAL_UINT32(512000u, sd_proto_address(false, 1000u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cmd0_frame_carries_the_crc_the_card_checks);
    RUN_TEST(test_cmd8_frame_carries_the_crc_the_card_checks);
    RUN_TEST(test_other_commands_carry_their_crc);
    RUN_TEST(test_r7_accepts_only_our_voltage_and_echoed_pattern);
    RUN_TEST(test_data_response_accepts_only_status_010);
    RUN_TEST(test_ocr_ccs_picks_block_or_byte_address);
    RUN_TEST(test_sector_address_is_block_number_or_byte_offset);
    return UNITY_END();
}
