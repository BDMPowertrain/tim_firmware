/*
 * Host tests for the BMS parameter table (App/bms/logic/Inc/bms_params.h,
 * ADR 0015): the table is usable, and each value is bounded as the spec
 * says.
 */
#include "bms_params.h"
#include "params.h"
#include "unity.h"

static params_t p;
static param_slot_t slots[BMS_PARAM_COUNT];

void setUp(void)
{
    params_load(&p, &bms_param_table, NULL, slots);
}

void tearDown(void) {}

static void test_the_table_is_usable_and_is_the_bmss(void)
{
    TEST_ASSERT_TRUE(params_table_ok(&bms_param_table));
    TEST_ASSERT_EQUAL_UINT32(PARAMS_IMAGE_BMS, bms_param_table.image);
    TEST_ASSERT_EQUAL(BMS_PARAM_COUNT, bms_param_table.count);
}

static void test_termination_defaults_open(void)
{
    TEST_ASSERT_EQUAL_INT32(0, params_value(&p, BMS_PARAM_CAN1_TERM));
    TEST_ASSERT_EQUAL_INT32(0, params_value(&p, BMS_PARAM_CAN2_TERM));
}

/* 3.60 to 4.18 V, unset 4.10 V, in mV. */
static void test_charge_target(void)
{
    TEST_ASSERT_EQUAL_INT32(4100, params_value(&p, BMS_PARAM_CHARGE_TARGET));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, BMS_PARAM_CHARGE_TARGET, 3600));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, BMS_PARAM_CHARGE_TARGET, 4180));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_CHARGE_TARGET, 3599));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_CHARGE_TARGET, 4181));
}

/* At most 12 A, unset 2 A, in 0.1 A. */
static void test_full_charge_limit(void)
{
    TEST_ASSERT_EQUAL_INT32(20, params_value(&p, BMS_PARAM_FULL_CHARGE_LIMIT));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, BMS_PARAM_FULL_CHARGE_LIMIT, 120));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_FULL_CHARGE_LIMIT, 121));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_FULL_CHARGE_LIMIT, -1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_table_is_usable_and_is_the_bmss);
    RUN_TEST(test_termination_defaults_open);
    RUN_TEST(test_charge_target);
    RUN_TEST(test_full_charge_limit);
    return UNITY_END();
}
