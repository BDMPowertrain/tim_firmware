/*
 * Host tests for decoding one Sweep (App/bms/logic/Inc/bq_sweep.h).
 *
 * Issue 62: "every 20 ms takes one Sweep of all 128 cell voltages and
 * temperatures ... a link timeout is reported as a missing measurement",
 * and the frame parsing is tested "including a bad CRC and a short
 * response".
 *
 * Layout (BQ79616-Q1 data sheet SLUSE81F 7.5.4.6): VCELL8_HI at 0x0578 down
 * to VCELL1_LO at 0x0587, TSREF at 0x058C, GPIO1_HI at 0x058E up to GPIO8_LO
 * at 0x059D, all big-endian two's complement; a cell LSB is 190.73 uV and a
 * GPIO LSB 152.59 uV. The top of the stack answers first.
 */
#include <string.h>

#include "bq_frame.h"
#include "bq_sweep.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* 3700 mV is 19399 cell LSBs (19399 x 190.73 uV = 3699.97 mV). */
#define RAW_3700_MV 19399u
/* 1526 mV is 10001 GPIO LSBs (10001 x 152.59 uV = 1526.05 mV). */
#define RAW_1526_MV 10001u

static uint8_t response[BQ_SWEEP_RESPONSE_LEN];

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* One device's 38 data bytes: VCELLn = cellRaw + n, GPIOk = gpioRaw + k. */
static size_t put_device(uint8_t *buf, uint8_t dev, uint16_t cellRaw, uint16_t gpioRaw)
{
    uint8_t data[BQ_SWEEP_COUNT];
    memset(data, 0, sizeof data);
    for (unsigned n = 1u; n <= 8u; n++) {
        put16(&data[(8u - n) * 2u], (uint16_t)(cellRaw + n));
    }
    for (unsigned k = 1u; k <= 8u; k++) {
        put16(&data[22u + (k - 1u) * 2u], (uint16_t)(gpioRaw + k));
    }

    buf[0] = BQ_SWEEP_COUNT - 1u;
    buf[1] = dev;
    put16(&buf[2], BQ_SWEEP_REG);
    memcpy(&buf[4], data, sizeof data);
    const uint16_t crc = bq_crc(buf, 4u + sizeof data);
    buf[4u + sizeof data] = (uint8_t)crc;
    buf[5u + sizeof data] = (uint8_t)(crc >> 8);
    return 6u + sizeof data;
}

/* Every device, top first, cells at 3700 mV and sensors at 1526 mV (plus
   the small per-channel offsets put_device adds). */
static size_t full_response(void)
{
    size_t len = 0u;
    for (uint8_t dev = BQ_SWEEP_DEVICES; dev >= 1u; dev--) {
        len += put_device(&response[len], dev, RAW_3700_MV - 8u, RAW_1526_MV - 8u);
    }
    return len;
}

static void test_a_full_response_is_704_bytes(void)
{
    TEST_ASSERT_EQUAL_UINT(704u, BQ_SWEEP_RESPONSE_LEN);
    TEST_ASSERT_EQUAL_UINT(BQ_SWEEP_RESPONSE_LEN, full_response());
}

static void test_the_sweep_read_is_one_stack_read_of_38_bytes_from_vcell8(void)
{
    uint8_t frame[BQ_FRAME_MAX];
    uint8_t expected[BQ_FRAME_MAX];
    const size_t len = bq_sweep_command(frame);

    TEST_ASSERT_EQUAL_UINT(bq_frame_read(expected, BQ_STACK_READ, 0u, 0x0578u, 38u), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, len);
}

static void test_a_full_response_gives_every_cell_and_sensor_in_place(void)
{
    bq_sweep_t s;
    const size_t len = full_response();

    bq_sweep_decode(response, len, &s);

    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, s.devices_read);
    /* Device 1 cell 8 and device 16 cell 8 carry the +8 offset: 3700 mV. */
    TEST_ASSERT_EQUAL_UINT16(3700u, s.cell_mv[7]);
    TEST_ASSERT_EQUAL_UINT16(3700u, s.cell_mv[127]);
    /* Cell 1 of device 3 is 7 LSBs lower: 19392 x 190.73 uV = 3698.6 mV. */
    TEST_ASSERT_EQUAL_UINT16(3699u, s.cell_mv[16]);
    /* GPIO8 of device 2 and of device 16: 1526 mV. */
    TEST_ASSERT_EQUAL_UINT16(1526u, s.temp_mv[15]);
    TEST_ASSERT_EQUAL_UINT16(1526u, s.temp_mv[127]);
    /* GPIO1 of device 1 is 7 LSBs lower: 9994 x 152.59 uV = 1525.0 mV. */
    TEST_ASSERT_EQUAL_UINT16(1525u, s.temp_mv[0]);
}

static void test_a_response_cut_short_by_a_timeout_misses_the_lower_devices(void)
{
    bq_sweep_t s;
    (void)full_response();
    /* Devices 16 down to 7 arrived; the read timed out in device 6's frame. */
    const size_t len = 10u * 44u + 20u;

    bq_sweep_decode(response, len, &s);

    TEST_ASSERT_EQUAL_HEX16(0xFFC0u, s.devices_read);
    TEST_ASSERT_EQUAL_UINT16(3700u, s.cell_mv[6u * 8u + 7u]);
    TEST_ASSERT_EQUAL_UINT16(0u, s.cell_mv[5u * 8u + 7u]);
    TEST_ASSERT_EQUAL_UINT16(0u, s.temp_mv[0]);
}

static void test_nothing_after_a_bad_crc_is_read(void)
{
    bq_sweep_t s;
    const size_t len = full_response();
    response[2u * 44u + 10u] ^= 0x04u; /* inside device 14's data */

    bq_sweep_decode(response, len, &s);

    TEST_ASSERT_EQUAL_HEX16(0xC000u, s.devices_read);
}

static void test_no_response_reads_nothing(void)
{
    bq_sweep_t s;
    memset(response, 0xFF, sizeof response);

    bq_sweep_decode(response, sizeof response, &s);
    TEST_ASSERT_EQUAL_HEX16(0x0000u, s.devices_read);

    bq_sweep_decode(response, 0u, &s);
    TEST_ASSERT_EQUAL_HEX16(0x0000u, s.devices_read);
    for (size_t i = 0u; i < BQ_SWEEP_CELLS; i++) {
        TEST_ASSERT_EQUAL_UINT16(0u, s.cell_mv[i]);
        TEST_ASSERT_EQUAL_UINT16(0u, s.temp_mv[i]);
    }
}

static void test_a_negative_or_unconverted_reading_is_zero(void)
{
    /* 0x8000 is the registers' reset value, before the first conversion. */
    bq_sweep_t s;
    const size_t len = put_device(response, 1u, 0x8000u - 1u, 0xFFF0u);

    bq_sweep_decode(response, len, &s);

    TEST_ASSERT_EQUAL_HEX16(0x0001u, s.devices_read);
    TEST_ASSERT_EQUAL_UINT16(0u, s.cell_mv[0]);
    TEST_ASSERT_EQUAL_UINT16(0u, s.temp_mv[0]);
}

static void test_a_frame_from_another_register_or_address_is_not_a_device(void)
{
    bq_sweep_t s;
    size_t len = put_device(response, 0u, RAW_3700_MV, RAW_1526_MV);   /* the bridge */
    len += put_device(&response[len], 17u, RAW_3700_MV, RAW_1526_MV); /* beyond the stack */
    response[len - 44u + 3u] = 0x79u; /* register 0x0579 */
    const uint16_t crc = bq_crc(&response[len - 44u], 42u);
    response[len - 2u] = (uint8_t)crc;
    response[len - 1u] = (uint8_t)(crc >> 8);

    bq_sweep_decode(response, len, &s);

    TEST_ASSERT_EQUAL_HEX16(0x0000u, s.devices_read);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_full_response_is_704_bytes);
    RUN_TEST(test_the_sweep_read_is_one_stack_read_of_38_bytes_from_vcell8);
    RUN_TEST(test_a_full_response_gives_every_cell_and_sensor_in_place);
    RUN_TEST(test_a_response_cut_short_by_a_timeout_misses_the_lower_devices);
    RUN_TEST(test_nothing_after_a_bad_crc_is_read);
    RUN_TEST(test_no_response_reads_nothing);
    RUN_TEST(test_a_negative_or_unconverted_reading_is_zero);
    RUN_TEST(test_a_frame_from_another_register_or_address_is_not_a_device);
    return UNITY_END();
}
