/*
 * Host tests for the VCU parameter table (App/vcu/logic/Inc/vcu_params.h,
 * ADR 0015): the table is usable, and its defaults never allow driving.
 */
#include "params.h"
#include "unity.h"
#include "vcu_params.h"

static params_t p;
static param_slot_t slots[VCU_PARAM_COUNT];

void setUp(void)
{
    params_load(&p, &vcu_param_table, NULL, slots);
}

void tearDown(void) {}

static void test_the_table_is_usable_and_is_the_vcus(void)
{
    TEST_ASSERT_TRUE(params_table_ok(&vcu_param_table));
    TEST_ASSERT_EQUAL_UINT32(PARAMS_IMAGE_VCU, vcu_param_table.image);
    TEST_ASSERT_EQUAL(VCU_PARAM_COUNT, vcu_param_table.count);
}

static void test_defaults_give_no_torque_and_open_termination(void)
{
    TEST_ASSERT_EQUAL_INT32(0, params_value(&p, VCU_PARAM_MAX_TORQUE));
    TEST_ASSERT_EQUAL_INT32(0, params_value(&p, VCU_PARAM_CAN1_TERM));
    TEST_ASSERT_EQUAL_INT32(0, params_value(&p, VCU_PARAM_CAN2_TERM));
}

/* A default calibration revision never matches the image's, so a board
   with no stored calibration is never Calibrated. */
static void test_the_default_calibration_revision_is_not_the_images(void)
{
    TEST_ASSERT_NOT_EQUAL(VCU_CALIBRATION_REVISION, params_value(&p, VCU_PARAM_CAL_REVISION));
}

/* The compile-time ceiling: 130 N.m, in the 0.1 N.m of INV_Command. */
static void test_maximum_torque_is_capped_at_130_nm(void)
{
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, VCU_PARAM_MAX_TORQUE, 1300));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, VCU_PARAM_MAX_TORQUE, 1301));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, VCU_PARAM_MAX_TORQUE, -1));
}

static void test_pedal_end_points_are_inside_the_12_bit_adc(void)
{
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, VCU_PARAM_APPS1_MAX, 4095));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, VCU_PARAM_APPS1_MAX, 4096));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, VCU_PARAM_APPS2_MIN, -1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_table_is_usable_and_is_the_vcus);
    RUN_TEST(test_defaults_give_no_torque_and_open_termination);
    RUN_TEST(test_the_default_calibration_revision_is_not_the_images);
    RUN_TEST(test_maximum_torque_is_capped_at_130_nm);
    RUN_TEST(test_pedal_end_points_are_inside_the_12_bit_adc);
    return UNITY_END();
}
