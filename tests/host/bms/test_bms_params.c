/*
 * Host tests for the BMS parameter table (App/bms/logic/Inc/bms_params.h,
 * ADR 0015): the table is usable, and each value is bounded as the spec
 * says.
 */
#include "bms_control.h"
#include "bms_params.h"
#include "cell_temp.h"
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

static const param_def_t *def(bms_param_t i)
{
    return &bms_param_table.defs[i];
}

/* Issue 66, ADR 0018: the discharge taper's break points, unset at the
   spec's 3.0 V, 2.7 V, 50 C and 57 C. */
static void test_the_discharge_tapers_default_to_the_spec(void)
{
    TEST_ASSERT_EQUAL_INT32(3000, params_value(&p, BMS_PARAM_DISCHARGE_V_FULL));
    TEST_ASSERT_EQUAL_INT32(2700, params_value(&p, BMS_PARAM_DISCHARGE_V_FLOOR));
    TEST_ASSERT_EQUAL_INT32(500, params_value(&p, BMS_PARAM_DISCHARGE_T_FULL));
    TEST_ASSERT_EQUAL_INT32(570, params_value(&p, BMS_PARAM_DISCHARGE_T_FLOOR));
}

/* "No setting can put a taper's end past its fault threshold": the lowest
   voltage floor is above the under-voltage fault, the highest temperature
   floor below the over-temperature fault. */
static void test_no_setting_puts_a_tapers_end_past_its_fault_threshold(void)
{
    int16_t otDeciC = 0;
    TEST_ASSERT_TRUE(cell_temp_from_mv(BMS_OT_MV, &otDeciC));

    TEST_ASSERT_GREATER_THAN_INT32((int32_t)BMS_UV_MV, def(BMS_PARAM_DISCHARGE_V_FLOOR)->min);
    TEST_ASSERT_LESS_THAN_INT32(otDeciC, def(BMS_PARAM_DISCHARGE_T_FLOOR)->max);

    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE,
                      params_set(&p, BMS_PARAM_DISCHARGE_V_FLOOR, (int32_t)BMS_UV_MV));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_DISCHARGE_T_FLOOR, otDeciC));
}

/* A taper's two points can never cross: every full point is clear of every
   floor point, so no setting makes a step or a taper that runs backwards. */
static void test_a_tapers_full_point_is_always_clear_of_its_floor(void)
{
    TEST_ASSERT_GREATER_THAN_INT32(def(BMS_PARAM_DISCHARGE_V_FLOOR)->max,
                                   def(BMS_PARAM_DISCHARGE_V_FULL)->min);
    TEST_ASSERT_LESS_THAN_INT32(def(BMS_PARAM_DISCHARGE_T_FLOOR)->min,
                                def(BMS_PARAM_DISCHARGE_T_FULL)->max);
}

/* micro-ohm, unset 0: no compensation, the taper acts earliest. */
static void test_cell_resistance(void)
{
    TEST_ASSERT_EQUAL_INT32(0, params_value(&p, BMS_PARAM_CELL_RESISTANCE));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, BMS_PARAM_CELL_RESISTANCE, 10000));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_CELL_RESISTANCE, 10001));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_CELL_RESISTANCE, -1));
}

/* 0.1 A: unset about 20 A, at most the cell datasheet maximum (250 A, a
   placeholder until the datasheet figure is in). */
static void test_full_discharge_limit(void)
{
    TEST_ASSERT_EQUAL_INT32(200, params_value(&p, BMS_PARAM_FULL_DISCHARGE_LIMIT));
    TEST_ASSERT_EQUAL(PARAMS_SET_OK, params_set(&p, BMS_PARAM_FULL_DISCHARGE_LIMIT, 2500));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_FULL_DISCHARGE_LIMIT, 2501));
    TEST_ASSERT_EQUAL(PARAMS_SET_OUT_OF_RANGE, params_set(&p, BMS_PARAM_FULL_DISCHARGE_LIMIT, -1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_table_is_usable_and_is_the_bmss);
    RUN_TEST(test_termination_defaults_open);
    RUN_TEST(test_charge_target);
    RUN_TEST(test_full_charge_limit);
    RUN_TEST(test_the_discharge_tapers_default_to_the_spec);
    RUN_TEST(test_no_setting_puts_a_tapers_end_past_its_fault_threshold);
    RUN_TEST(test_a_tapers_full_point_is_always_clear_of_its_floor);
    RUN_TEST(test_cell_resistance);
    RUN_TEST(test_full_discharge_limit);
    return UNITY_END();
}
