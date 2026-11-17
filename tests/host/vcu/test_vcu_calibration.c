/*
 * Host tests for the Calibrated rule and the guided capture
 * (App/vcu/logic/Inc/vcu_calibration.h), issue 59.
 *
 * ADR 0016: "Calibrated" means a valid parameter copy holds every end
 * point; each sensor has minimum below maximum, a span above a minimum
 * size and end points inside its electrically valid window; and the
 * stored calibration revision equals the image's. End points are captured
 * by a guided command (pedal released, then fully pressed; both sensors
 * must move together) and a failing set is refused with the reason.
 *
 * Counts by hand from the 0.6 divider into a 12-bit, 3.3 V ADC
 * (vcu_sensors.h): 0.25 V is 186 counts, 4.75 V is 3536, 1 V is 744.
 */
#include <string.h>

#include "adc_window.h"
#include "unity.h"
#include "vcu_calibration.h"
#include "vcu_params.h"

static vcu_cal_values_t good(void)
{
    return (vcu_cal_values_t){
        .apps_min = { 600, 700 },
        .apps_max = { 2600, 2900 },
        .brake_zero = { 400, 420 },
        .brake_engaged = 150,
        .revision = VCU_CALIBRATION_REVISION,
    };
}

static void assert_result(vcu_cal_reason_t reason, vcu_cal_sensor_t sensor, vcu_cal_result_t r)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(reason, r.reason, vcu_cal_reason_name(r.reason));
    TEST_ASSERT_EQUAL_INT(sensor, r.sensor);
}

void setUp(void) {}
void tearDown(void) {}

/* ---- the Calibrated rule ---------------------------------------------- */

static void test_a_good_set_is_calibrated(void)
{
    const vcu_cal_values_t v = good();
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));
}

static void test_values_not_from_a_valid_copy_are_not_calibrated(void)
{
    const vcu_cal_values_t v = good();
    assert_result(VCU_CAL_MISSING, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, false));
}

static void test_another_revision_is_not_calibrated(void)
{
    vcu_cal_values_t v = good();

    v.revision = 0;
    assert_result(VCU_CAL_REVISION, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));
    v.revision = VCU_CALIBRATION_REVISION + 1;
    assert_result(VCU_CAL_REVISION, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));
}

static void test_minimum_not_below_maximum_is_not_calibrated(void)
{
    vcu_cal_values_t v = good();

    v.apps_min[1] = 2900;
    assert_result(VCU_CAL_APPS_ORDER, VCU_CAL_SENSOR_APPS2, vcu_cal_check(&v, true));
    v.apps_min[1] = 3000;
    assert_result(VCU_CAL_APPS_ORDER, VCU_CAL_SENSOR_APPS2, vcu_cal_check(&v, true));
}

static void test_span_below_one_volt_is_not_calibrated(void)
{
    vcu_cal_values_t v = good();

    v.apps_max[0] = 600 + 743;
    assert_result(VCU_CAL_APPS_SPAN, VCU_CAL_SENSOR_APPS1, vcu_cal_check(&v, true));
    v.apps_max[0] = 600 + 744;
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));
}

static void test_apps_end_point_outside_the_window_is_not_calibrated(void)
{
    vcu_cal_values_t v = good();

    v.apps_min[0] = 185;
    assert_result(VCU_CAL_APPS_WINDOW, VCU_CAL_SENSOR_APPS1, vcu_cal_check(&v, true));
    v.apps_min[0] = 186;
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));

    v = good();
    v.apps_max[1] = 3537;
    assert_result(VCU_CAL_APPS_WINDOW, VCU_CAL_SENSOR_APPS2, vcu_cal_check(&v, true));
    v.apps_max[1] = 3536;
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));
}

static void test_brake_zero_outside_the_window_is_not_calibrated(void)
{
    vcu_cal_values_t v = good();

    v.brake_zero[1] = 185;
    assert_result(VCU_CAL_BRAKE_WINDOW, VCU_CAL_SENSOR_BRAKE_REAR, vcu_cal_check(&v, true));

    /* The threshold must be reachable inside the window too. */
    v = good();
    v.brake_zero[0] = 3536 - 149;
    assert_result(VCU_CAL_BRAKE_WINDOW, VCU_CAL_SENSOR_BRAKE_FRONT, vcu_cal_check(&v, true));
    v.brake_zero[0] = 3536 - 150;
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));
}

static void test_brake_threshold_below_its_minimum_is_not_calibrated(void)
{
    vcu_cal_values_t v = good();

    v.brake_engaged = 74;
    assert_result(VCU_CAL_BRAKE_THRESHOLD, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));
    v.brake_engaged = 75;
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE, vcu_cal_check(&v, true));
}

/* ---- the guided capture ---------------------------------------------- */

static vcu_cal_capture_t released;
static vcu_cal_capture_t floored;
static vcu_cal_capture_t braked;

/* n samples of one position, every sensor at these counts. */
static void hold(vcu_cal_capture_t *c, uint16_t apps1, uint16_t apps2, uint16_t front,
                 uint16_t rear, unsigned n)
{
    const vcu_sensor_raw_t raw = {
        .apps = { apps1, apps2 },
        .brake = { front, rear },
        .supply = { 3722u, 3722u },
    };

    for (unsigned i = 0; i < n; i++) {
        vcu_cal_capture_add(c, &raw);
    }
}

static void capture_good(void)
{
    vcu_cal_capture_init(&released);
    vcu_cal_capture_init(&floored);
    vcu_cal_capture_init(&braked);
    hold(&released, 600u, 2900u, 400u, 410u, 50u);
    hold(&floored, 2600u, 700u, 400u, 410u, 50u);
    hold(&braked, 600u, 2900u, 1000u, 810u, 50u);
}

static void test_capture_gives_the_end_points_and_a_quarter_of_the_smaller_brake_rise(void)
{
    vcu_cal_values_t v;

    capture_good();
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));
    TEST_ASSERT_EQUAL_INT32(600, v.apps_min[0]);
    TEST_ASSERT_EQUAL_INT32(2600, v.apps_max[0]);
    TEST_ASSERT_EQUAL_INT32(700, v.apps_min[1]);  /* APPS2 falls: its min is floored */
    TEST_ASSERT_EQUAL_INT32(2900, v.apps_max[1]);
    TEST_ASSERT_EQUAL_INT32(400, v.brake_zero[0]);
    TEST_ASSERT_EQUAL_INT32(410, v.brake_zero[1]);
    TEST_ASSERT_EQUAL_INT32(100, v.brake_engaged); /* rises 600 and 400: 25% of 400 */
    TEST_ASSERT_EQUAL_INT32(VCU_CALIBRATION_REVISION, v.revision);
}

static void test_capture_averages_each_position(void)
{
    vcu_cal_values_t v;

    capture_good();
    vcu_cal_capture_init(&released);
    hold(&released, 590u, 2890u, 395u, 405u, 1u);
    hold(&released, 610u, 2910u, 405u, 415u, 1u);
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));
    TEST_ASSERT_EQUAL_INT32(600, v.apps_min[0]);
    TEST_ASSERT_EQUAL_INT32(2900, v.apps_max[1]);
    TEST_ASSERT_EQUAL_INT32(400, v.brake_zero[0]);
}

static void test_capture_with_a_missing_sample_is_refused(void)
{
    vcu_cal_values_t v;

    capture_good();
    hold(&floored, 2600u, ADC_WINDOW_EMPTY, 400u, 410u, 1u);
    assert_result(VCU_CAL_NO_SAMPLE, VCU_CAL_SENSOR_NONE,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));

    capture_good();
    vcu_cal_capture_init(&braked); /* no samples at all */
    assert_result(VCU_CAL_NO_SAMPLE, VCU_CAL_SENSOR_NONE,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));
}

static void test_capture_of_a_moving_sensor_is_refused(void)
{
    vcu_cal_values_t v;

    capture_good();
    hold(&floored, 2640u, 700u, 400u, 410u, 1u); /* 40 counts: still steady */
    assert_result(VCU_CAL_OK, VCU_CAL_SENSOR_NONE,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));

    hold(&floored, 2641u, 700u, 400u, 410u, 1u); /* 41 counts */
    assert_result(VCU_CAL_NOT_STEADY, VCU_CAL_SENSOR_APPS1,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));
}

static void test_capture_where_one_sensor_did_not_move_is_refused(void)
{
    vcu_cal_values_t v;

    capture_good();
    vcu_cal_capture_init(&floored);
    hold(&floored, 2600u, 2900u, 400u, 410u, 50u); /* APPS2 stayed put */
    assert_result(VCU_CAL_APPS_DIRECTION, VCU_CAL_SENSOR_APPS2,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));
}

static void test_capture_where_a_sensor_moved_the_wrong_way_is_refused(void)
{
    vcu_cal_values_t v;

    capture_good();
    vcu_cal_capture_init(&floored);
    hold(&floored, 400u, 700u, 400u, 410u, 50u); /* APPS1 fell */
    assert_result(VCU_CAL_APPS_DIRECTION, VCU_CAL_SENSOR_APPS1,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));
}

static void test_capture_that_would_not_be_calibrated_is_refused_with_the_reason(void)
{
    vcu_cal_values_t v;

    /* APPS1 moves only 500 counts. */
    capture_good();
    vcu_cal_capture_init(&floored);
    hold(&floored, 1100u, 700u, 400u, 410u, 50u);
    assert_result(VCU_CAL_APPS_SPAN, VCU_CAL_SENSOR_APPS1,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));

    /* The rear brake rises 299 counts: a quarter is 74, below 75. */
    capture_good();
    vcu_cal_capture_init(&braked);
    hold(&braked, 600u, 2900u, 1000u, 709u, 50u);
    assert_result(VCU_CAL_BRAKE_THRESHOLD, VCU_CAL_SENSOR_NONE,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));

    /* A brake that did not rise at all. */
    capture_good();
    vcu_cal_capture_init(&braked);
    hold(&braked, 600u, 2900u, 1000u, 410u, 50u);
    assert_result(VCU_CAL_BRAKE_THRESHOLD, VCU_CAL_SENSOR_NONE,
                  vcu_cal_from_captures(&released, &floored, &braked, &v));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_good_set_is_calibrated);
    RUN_TEST(test_values_not_from_a_valid_copy_are_not_calibrated);
    RUN_TEST(test_another_revision_is_not_calibrated);
    RUN_TEST(test_minimum_not_below_maximum_is_not_calibrated);
    RUN_TEST(test_span_below_one_volt_is_not_calibrated);
    RUN_TEST(test_apps_end_point_outside_the_window_is_not_calibrated);
    RUN_TEST(test_brake_zero_outside_the_window_is_not_calibrated);
    RUN_TEST(test_brake_threshold_below_its_minimum_is_not_calibrated);
    RUN_TEST(test_capture_gives_the_end_points_and_a_quarter_of_the_smaller_brake_rise);
    RUN_TEST(test_capture_averages_each_position);
    RUN_TEST(test_capture_with_a_missing_sample_is_refused);
    RUN_TEST(test_capture_of_a_moving_sensor_is_refused);
    RUN_TEST(test_capture_where_one_sensor_did_not_move_is_refused);
    RUN_TEST(test_capture_where_a_sensor_moved_the_wrong_way_is_refused);
    RUN_TEST(test_capture_that_would_not_be_calibrated_is_refused_with_the_reason);
    return UNITY_END();
}
