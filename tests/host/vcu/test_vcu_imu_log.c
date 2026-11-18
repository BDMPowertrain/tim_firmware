/*
 * Host tests for the IMU Records' pairing (App/vcu/logic/Inc/vcu_imu_log.h),
 * issue 61: "IMU Records (accel and gyro) at 100 Hz from the IMU task".
 *
 * The FIFO is read about every 100 ms and holds both sensors at 104 Hz,
 * interleaved. Times are worked by hand: one ODR period is 9.615 ms.
 */
#include "unity.h"
#include "vcu_imu_log.h"

static vcu_imu_pairer_t p;
static vcu_imu_record_t pairs[VCU_IMU_PAIRS_MAX];

void setUp(void)
{
    vcu_imu_pairer_init(&p);
}

void tearDown(void) {}

static vcu_imu_entry_t accel(int16_t x)
{
    return (vcu_imu_entry_t){.sensor = VCU_IMU_ACCEL, .xyz = {x, (int16_t)(x + 1), (int16_t)(x + 2)}};
}

static vcu_imu_entry_t gyro(int16_t x)
{
    return (vcu_imu_entry_t){.sensor = VCU_IMU_GYRO, .xyz = {x, (int16_t)(x + 1), (int16_t)(x + 2)}};
}

static void test_interleaved_samples_make_one_record_each_back_dated_one_period_apart(void)
{
    const vcu_imu_entry_t e[6] = {accel(10), gyro(20), accel(30), gyro(40), accel(50), gyro(60)};

    TEST_ASSERT_EQUAL_size_t(3u, vcu_imu_pair(&p, e, 6u, 0u, 1000u, pairs));
    TEST_ASSERT_EQUAL_INT16(10, pairs[0].sample.accel[0]);
    TEST_ASSERT_EQUAL_INT16(22, pairs[0].sample.gyro[2]);
    TEST_ASSERT_EQUAL_INT16(50, pairs[2].sample.accel[0]);
    TEST_ASSERT_EQUAL_INT16(60, pairs[2].sample.gyro[0]);
    /* The newest at the read, the others 9.6 and 19.2 ms before, rounded. */
    TEST_ASSERT_EQUAL_UINT32(981u, pairs[0].uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(990u, pairs[1].uptime_ms);
    TEST_ASSERT_EQUAL_UINT32(1000u, pairs[2].uptime_ms);
}

static void test_the_order_within_a_pair_does_not_matter(void)
{
    const vcu_imu_entry_t e[2] = {gyro(7), accel(8)};

    TEST_ASSERT_EQUAL_size_t(1u, vcu_imu_pair(&p, e, 2u, 0u, 50u, pairs));
    TEST_ASSERT_EQUAL_INT16(8, pairs[0].sample.accel[0]);
    TEST_ASSERT_EQUAL_INT16(7, pairs[0].sample.gyro[0]);
}

static void test_a_pair_split_across_two_reads_is_completed_by_the_second(void)
{
    const vcu_imu_entry_t first[3] = {accel(1), gyro(2), accel(3)};
    const vcu_imu_entry_t second[1] = {gyro(4)};

    TEST_ASSERT_EQUAL_size_t(1u, vcu_imu_pair(&p, first, 3u, 0u, 100u, pairs));
    TEST_ASSERT_EQUAL_size_t(1u, vcu_imu_pair(&p, second, 1u, 0u, 200u, pairs));
    TEST_ASSERT_EQUAL_INT16(3, pairs[0].sample.accel[0]);
    TEST_ASSERT_EQUAL_INT16(4, pairs[0].sample.gyro[0]);
    TEST_ASSERT_EQUAL_UINT32(200u, pairs[0].uptime_ms);
}

static void test_a_sample_whose_partner_never_came_is_replaced_by_the_newer_one(void)
{
    const vcu_imu_entry_t e[3] = {accel(1), accel(5), gyro(9)};

    TEST_ASSERT_EQUAL_size_t(1u, vcu_imu_pair(&p, e, 3u, 0u, 10u, pairs));
    TEST_ASSERT_EQUAL_INT16(5, pairs[0].sample.accel[0]);
}

static void test_entries_of_another_sensor_are_ignored(void)
{
    const vcu_imu_entry_t e[3] = {accel(1), {.sensor = 0x19u}, gyro(2)};

    TEST_ASSERT_EQUAL_size_t(1u, vcu_imu_pair(&p, e, 3u, 0u, 10u, pairs));
}

static void test_a_full_fifo_fits_the_pairs_array(void)
{
    vcu_imu_entry_t e[VCU_IMU_ENTRIES_MAX];

    for (size_t i = 0; i < VCU_IMU_ENTRIES_MAX; i++) {
        e[i] = (i % 2u == 0u) ? accel((int16_t)i) : gyro((int16_t)i);
    }
    TEST_ASSERT_EQUAL_size_t(VCU_IMU_ENTRIES_MAX / 2u,
                             vcu_imu_pair(&p, e, VCU_IMU_ENTRIES_MAX, 0u, 100000u, pairs));
}

static void test_entries_left_in_the_fifo_date_this_read_further_back(void)
{
    const vcu_imu_entry_t e[2] = {accel(1), gyro(2)};

    /* Four entries (two pairs) still in the FIFO: this read's pair is two
       periods, 19.2 ms, older than the read. */
    TEST_ASSERT_EQUAL_size_t(1u, vcu_imu_pair(&p, e, 2u, 4u, 1000u, pairs));
    TEST_ASSERT_EQUAL_UINT32(981u, pairs[0].uptime_ms);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_interleaved_samples_make_one_record_each_back_dated_one_period_apart);
    RUN_TEST(test_the_order_within_a_pair_does_not_matter);
    RUN_TEST(test_a_pair_split_across_two_reads_is_completed_by_the_second);
    RUN_TEST(test_a_sample_whose_partner_never_came_is_replaced_by_the_newer_one);
    RUN_TEST(test_entries_of_another_sensor_are_ignored);
    RUN_TEST(test_a_full_fifo_fits_the_pairs_array);
    RUN_TEST(test_entries_left_in_the_fifo_date_this_read_further_back);
    return UNITY_END();
}
