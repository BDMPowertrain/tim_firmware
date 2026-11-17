/*
 * Host tests for the VCU's pedal and brake sensors
 * (App/vcu/logic/Inc/vcu_sensors.h).
 *
 * Spec (issue 55): "Each APPS is scaled to 0-100% from its own stored
 * minimum and maximum; scaling and the out-of-range margin are logic and
 * host-tested." "A sensor supply outside its window marks that sensor
 * failed." Issue 20: APPS1 rises with the pedal and APPS2 falls; supply 1
 * feeds APPS1 and the front brake sensor, supply 2 APPS2 and the rear.
 *
 * Counts used below, through the 0.6 divider on a 3.3 V reference:
 * 5.00 V = 3722, 4.75 V = 3536.6, 5.25 V = 3908.9, 0.25 V = 186.1.
 */
#include "adc_window.h"
#include "unity.h"
#include "vcu_sensors.h"

static vcu_sensor_raw_t raw;
static vcu_sensor_cal_t cal;
static vcu_sensors_t out;

/* A healthy car at rest: both supplies at 5 V, both APPS calibrated over
   1000-3000 counts and released, both brakes at a 0.5 V zero. */
void setUp(void)
{
    raw = (vcu_sensor_raw_t){
        .apps = { 1000u, 3000u },
        .brake = { 372u, 372u },
        .supply = { 3722u, 3722u },
    };
    cal = (vcu_sensor_cal_t){
        .apps_min = { 1000, 1000 },
        .apps_max = { 3000, 3000 },
        .brake_zero = { 372, 372 },
    };
}

void tearDown(void) {}

static void compute(void)
{
    vcu_sensors_compute(&raw, &cal, &out);
}

/* T.4.2.1: the pedal travel is the driver's request, computed from each
   sensor on its own. */
static void test_apps1_rises_from_its_min_to_its_max(void)
{
    raw.apps[0] = 1000u;
    compute();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[0].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.apps[0].travel);

    raw.apps[0] = 2000u;
    compute();
    TEST_ASSERT_EQUAL_UINT16(500u, out.apps[0].travel); /* 50.0% */

    raw.apps[0] = 3000u;
    compute();
    TEST_ASSERT_EQUAL_UINT16(1000u, out.apps[0].travel);
}

/* Issue 20: APPS2 has the inverted transfer, so 0% is its higher count.
   Its stored min and max are still the lower and higher counts. */
static void test_apps2_falls_from_its_max_to_its_min(void)
{
    raw.apps[1] = 3000u;
    compute();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[1].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.apps[1].travel);

    raw.apps[1] = 2500u;
    compute();
    TEST_ASSERT_EQUAL_UINT16(250u, out.apps[1].travel);

    raw.apps[1] = 1000u;
    compute();
    TEST_ASSERT_EQUAL_UINT16(1000u, out.apps[1].travel);
}

/* Each sensor from its own end points, not the other's. */
static void test_each_apps_scales_from_its_own_end_points(void)
{
    cal.apps_min[0] = 500;
    cal.apps_max[0] = 1500;
    raw.apps[0] = 1000u;
    raw.apps[1] = 2000u; /* APPS2 on 1000-3000 */

    compute();

    TEST_ASSERT_EQUAL_UINT16(500u, out.apps[0].travel);
    TEST_ASSERT_EQUAL_UINT16(500u, out.apps[1].travel);
}

/* T.4.2.10: inside 10% of the span beyond an end point the sensor is
   still in range, and its travel clamps to 0 or 100%. The span here is
   2000 counts, so the margin is 200. */
static void test_apps_inside_the_margin_clamps_to_0_or_100(void)
{
    raw.apps[0] = 800u;  /* 200 below min */
    raw.apps[1] = 800u;  /* 200 below min: past 100% for the falling APPS2 */
    compute();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[0].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.apps[0].travel);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[1].status);
    TEST_ASSERT_EQUAL_UINT16(1000u, out.apps[1].travel);

    raw.apps[0] = 3200u; /* 200 above max */
    raw.apps[1] = 3200u;
    compute();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[0].status);
    TEST_ASSERT_EQUAL_UINT16(1000u, out.apps[0].travel);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[1].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.apps[1].travel);
}

/* T.4.2.10: past the margin the sensor is out of range and its travel is
   not used. An open APPS1 (pulled down to 0) and an open APPS2 (pulled up
   to the top) both land here. */
static void test_apps_past_the_margin_is_out_of_range(void)
{
    raw.apps[0] = 799u;
    raw.apps[1] = 3201u;
    compute();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OUT_OF_RANGE, out.apps[0].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.apps[0].travel);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OUT_OF_RANGE, out.apps[1].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.apps[1].travel);

    raw.apps[0] = 0u;
    raw.apps[1] = 4095u;
    compute();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OUT_OF_RANGE, out.apps[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OUT_OF_RANGE, out.apps[1].status);
}

/* ADR 0015: the default end points are all 0, which never allow driving.
   With min not below max there is no span to scale with. */
static void test_default_end_points_are_not_calibrated(void)
{
    cal = (vcu_sensor_cal_t){0};

    compute();

    TEST_ASSERT_EQUAL(VCU_SENSOR_NOT_CALIBRATED, out.apps[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_NOT_CALIBRATED, out.apps[1].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.apps[0].travel);
    TEST_ASSERT_EQUAL(VCU_SENSOR_NOT_CALIBRATED, out.brake[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_NOT_CALIBRATED, out.brake[1].status);
}

static void test_min_above_max_is_not_calibrated(void)
{
    cal.apps_min[1] = 3000;
    cal.apps_max[1] = 1000;

    compute();

    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_NOT_CALIBRATED, out.apps[1].status);
}

/* Before the ADC has a sample (or if it never started) nothing is
   usable, whatever the calibration. */
static void test_no_sample_is_never_ok(void)
{
    raw = (vcu_sensor_raw_t){
        .apps = { ADC_WINDOW_EMPTY, ADC_WINDOW_EMPTY },
        .brake = { ADC_WINDOW_EMPTY, ADC_WINDOW_EMPTY },
        .supply = { ADC_WINDOW_EMPTY, ADC_WINDOW_EMPTY },
    };

    compute();

    TEST_ASSERT_EQUAL(VCU_SENSOR_NO_SAMPLE, out.apps[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_NO_SAMPLE, out.apps[1].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_NO_SAMPLE, out.brake[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_NO_SAMPLE, out.brake[1].status);
    TEST_ASSERT_FALSE(out.supply_ok[0]);
    TEST_ASSERT_FALSE(out.supply_ok[1]);
}

/* Issue 20, T.4.2.2: a short in one sensor harness takes out only that
   sensor's supply, and firmware sees it as that sensor failing. Supply 1
   feeds APPS1 and the front brake sensor. */
static void test_a_low_supply_fails_the_two_sensors_it_feeds(void)
{
    raw.supply[0] = 3400u; /* about 4.57 V */

    compute();

    TEST_ASSERT_FALSE(out.supply_ok[0]);
    TEST_ASSERT_TRUE(out.supply_ok[1]);
    TEST_ASSERT_EQUAL(VCU_SENSOR_SUPPLY_FAULT, out.apps[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_SUPPLY_FAULT, out.brake[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[1].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.brake[1].status);
}

/* Supply 2 feeds APPS2 and the rear brake sensor. */
static void test_a_high_supply_fails_the_two_sensors_it_feeds(void)
{
    raw.supply[1] = 4000u; /* about 5.37 V */
    raw.apps[1] = 2000u;

    compute();

    TEST_ASSERT_TRUE(out.supply_ok[0]);
    TEST_ASSERT_FALSE(out.supply_ok[1]);
    TEST_ASSERT_EQUAL(VCU_SENSOR_SUPPLY_FAULT, out.apps[1].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.apps[1].travel);
    TEST_ASSERT_EQUAL(VCU_SENSOR_SUPPLY_FAULT, out.brake[1].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.apps[0].status);
}

/* The window is 4.75-5.25 V: 3536.6 to 3908.9 counts. */
static void test_the_supply_window_edges(void)
{
    raw.supply[0] = 3537u;
    raw.supply[1] = 3908u;
    compute();
    TEST_ASSERT_TRUE(out.supply_ok[0]);
    TEST_ASSERT_TRUE(out.supply_ok[1]);

    raw.supply[0] = 3535u;
    raw.supply[1] = 3910u;
    compute();
    TEST_ASSERT_FALSE(out.supply_ok[0]);
    TEST_ASSERT_FALSE(out.supply_ok[1]);
}

/* Issue 55: brake pressure from the stored zero, in counts above it.
   Below the zero (noise at rest) reads as no pressure. */
static void test_brake_pressure_is_counts_above_its_own_zero(void)
{
    cal.brake_zero[1] = 400;
    raw.brake[0] = 1372u;
    raw.brake[1] = 390u;

    compute();

    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.brake[0].status);
    TEST_ASSERT_EQUAL_UINT16(1000u, out.brake[0].pressure);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.brake[1].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.brake[1].pressure);
}

/* T.4.3.4, issue 20: an open brake sensor reads out of range. The window
   is 0.25-4.75 V: 186.1 to 3536.6 counts. */
static void test_brake_outside_its_window_is_out_of_range(void)
{
    raw.brake[0] = 187u;
    raw.brake[1] = 3536u;
    compute();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.brake[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OK, out.brake[1].status);

    raw.brake[0] = 0u;    /* open, pulled down */
    raw.brake[1] = 3700u; /* shorted to the 5 V supply */
    compute();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OUT_OF_RANGE, out.brake[0].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.brake[0].pressure);
    TEST_ASSERT_EQUAL(VCU_SENSOR_OUT_OF_RANGE, out.brake[1].status);
    TEST_ASSERT_EQUAL_UINT16(0u, out.brake[1].pressure);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_brake_pressure_is_counts_above_its_own_zero);
    RUN_TEST(test_brake_outside_its_window_is_out_of_range);
    RUN_TEST(test_a_low_supply_fails_the_two_sensors_it_feeds);
    RUN_TEST(test_a_high_supply_fails_the_two_sensors_it_feeds);
    RUN_TEST(test_the_supply_window_edges);
    RUN_TEST(test_default_end_points_are_not_calibrated);
    RUN_TEST(test_min_above_max_is_not_calibrated);
    RUN_TEST(test_no_sample_is_never_ok);
    RUN_TEST(test_apps_inside_the_margin_clamps_to_0_or_100);
    RUN_TEST(test_apps_past_the_margin_is_out_of_range);
    RUN_TEST(test_apps2_falls_from_its_max_to_its_min);
    RUN_TEST(test_each_apps_scales_from_its_own_end_points);
    RUN_TEST(test_apps1_rises_from_its_min_to_its_max);
    return UNITY_END();
}
