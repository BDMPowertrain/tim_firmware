/*
 * Host tests for Derating, the Discharge limit (App/bms/logic/Inc/
 * bms_derate.h; ADR 0018; spec #36, "BMS image"; issue 66).
 *
 * "Two linear tapers, the lower wins: 100% at 3.0 V to 20% at 2.7 V on
 * sag-compensated minimum cell voltage (measured + pack current x cell
 * resistance; raw voltage when pack current is invalid), and 100% at 50 C
 * to 20% at 57 C. It falls at up to 200%/s and rises at up to 10%/s."
 *
 * The expected limits below are worked by hand from those words, with a
 * full limit of 100.0 A so a percentage reads straight off as amps.
 */
#include "bms_derate.h"
#include "unity.h"

static const bms_derate_config_t spec = {
    .v_full_mv = 3000u,
    .v_floor_mv = 2700u,
    .t_full_dc = 500,
    .t_floor_dc = 570,
    .cell_res_uohm = 1000u, /* 1 mOhm: 100 A puts back 100 mV */
    .full_limit_da = 1000u, /* 100.0 A */
};

static const bms_current_t none = { .deci_a = 0, .valid = true };

#define COOL 250 /* 25.0 C: no temperature taper */
#define FULL 3600u /* a full cell: no voltage taper */

static bms_derate_t d;

void setUp(void)
{
    bms_derate_init(&d, &spec);
}

void tearDown(void) {}

/* The first measurement sets the limit to the target at once. */
static uint16_t first(uint16_t mv, int16_t dc, bms_current_t i)
{
    return bms_derate_step(&d, 0u, true, mv, dc, i, true);
}

static void test_a_full_cool_pack_gets_the_full_limit(void)
{
    TEST_ASSERT_EQUAL_UINT16(1000u, first(FULL, COOL, none));
}

static void test_the_voltage_taper_runs_from_100_percent_at_3v0_to_20_percent_at_2v7(void)
{
    TEST_ASSERT_EQUAL_UINT16(1000u, first(3000u, COOL, none));
    setUp();
    TEST_ASSERT_EQUAL_UINT16(600u, first(2850u, COOL, none)); /* halfway: 60% */
    setUp();
    TEST_ASSERT_EQUAL_UINT16(200u, first(2700u, COOL, none));
    setUp();
    TEST_ASSERT_EQUAL_UINT16(200u, first(2600u, COOL, none)); /* held at 20% */
}

static void test_the_temperature_taper_runs_from_100_percent_at_50c_to_20_percent_at_57c(void)
{
    TEST_ASSERT_EQUAL_UINT16(1000u, first(FULL, 500, none));
    setUp();
    TEST_ASSERT_EQUAL_UINT16(600u, first(FULL, 535, none)); /* 53.5 C: halfway */
    setUp();
    TEST_ASSERT_EQUAL_UINT16(200u, first(FULL, 570, none));
    setUp();
    TEST_ASSERT_EQUAL_UINT16(200u, first(FULL, 600, none)); /* held at 20% */
}

static void test_the_lower_taper_wins(void)
{
    /* Voltage 60% (2.85 V), temperature 36% (55.6 C): 36%. */
    TEST_ASSERT_EQUAL_UINT16(360u, first(2850u, 556, none));
    setUp();
    /* Voltage 40% (2.775 V), temperature 60% (53.5 C): 40%. */
    TEST_ASSERT_EQUAL_UINT16(400u, first(2775u, 535, none));
}

/* 2.80 V measured at 100 A discharge with 1 mOhm per cell: 2.90 V
   compensated, so 2/3 of the way from 20% to 100%, 73.3%. */
static void test_the_voltage_taper_adds_the_sag_back(void)
{
    const bms_current_t discharging = { .deci_a = 1000, .valid = true };
    TEST_ASSERT_EQUAL_UINT16(733u, first(2800u, COOL, discharging));
}

/* The same reading with the current invalid tapers on the raw 2.80 V: 46.7%. */
static void test_with_pack_current_invalid_the_taper_uses_the_raw_voltage(void)
{
    const bms_current_t invalid = { .deci_a = 1000, .valid = false };
    TEST_ASSERT_EQUAL_UINT16(466u, first(2800u, COOL, invalid));
}

/* Steps of 20 ms, one Sweep each, from t ms; returns the last limit. */
static uint16_t run(uint32_t *t, int steps, uint16_t mv, int16_t dc)
{
    uint16_t limit = 0u;
    for (int i = 0; i < steps; i++) {
        *t += 20u;
        limit = bms_derate_step(&d, *t, true, mv, dc, none, true);
    }
    return limit;
}

/* 200%/s is 4% a 20 ms step: from 100% to a 20% target takes 20 steps. */
static void test_the_limit_falls_at_200_percent_a_second(void)
{
    uint32_t t = 0u;
    TEST_ASSERT_EQUAL_UINT16(1000u, first(FULL, COOL, none));
    TEST_ASSERT_EQUAL_UINT16(960u, run(&t, 1, 2600u, COOL));
    TEST_ASSERT_EQUAL_UINT16(240u, run(&t, 18, 2600u, COOL));
    TEST_ASSERT_EQUAL_UINT16(200u, run(&t, 1, 2600u, COOL));
    TEST_ASSERT_EQUAL_UINT16(200u, run(&t, 5, 2600u, COOL));
}

/* 10%/s is 0.2% a 20 ms step: from 20% to 100% takes 8 s. */
static void test_the_limit_rises_at_10_percent_a_second(void)
{
    uint32_t t = 0u;
    TEST_ASSERT_EQUAL_UINT16(200u, first(2600u, COOL, none));
    TEST_ASSERT_EQUAL_UINT16(202u, run(&t, 1, FULL, COOL));
    TEST_ASSERT_EQUAL_UINT16(300u, run(&t, 49, FULL, COOL)); /* 1 s */
    TEST_ASSERT_EQUAL_UINT16(998u, run(&t, 349, FULL, COOL));
    TEST_ASSERT_EQUAL_UINT16(1000u, run(&t, 1, FULL, COOL));
    TEST_ASSERT_EQUAL_UINT16(1000u, run(&t, 5, FULL, COOL));
}

/* A step with no measurement holds the limit, and the next one moves it
   by its elapsed time, no further than a second's worth. */
static void test_with_no_measurement_the_limit_holds(void)
{
    TEST_ASSERT_EQUAL_UINT16(1000u, first(FULL, COOL, none));
    TEST_ASSERT_EQUAL_UINT16(1000u, bms_derate_step(&d, 20u, false, 2600u, COOL, none, true));
    /* 40 ms since the last measurement: 8%. */
    TEST_ASSERT_EQUAL_UINT16(920u, bms_derate_step(&d, 40u, true, 2600u, COOL, none, true));

    setUp();
    TEST_ASSERT_EQUAL_UINT16(200u, first(2600u, COOL, none));
    /* 10 s with none: a rise of 10%, not 100%. */
    TEST_ASSERT_EQUAL_UINT16(300u, bms_derate_step(&d, 10000u, true, FULL, COOL, none, true));
}

/* Spec #36: "the BMS sends zero limits until BMS_OK is up". The limit
   follows the cells meanwhile, so BMS_OK rising gives it at once. */
static void test_the_limit_is_zero_until_bms_ok_is_up(void)
{
    TEST_ASSERT_EQUAL_UINT16(0u, bms_derate_step(&d, 0u, true, FULL, COOL, none, false));
    TEST_ASSERT_EQUAL_UINT16(0u, bms_derate_step(&d, 20u, true, FULL, COOL, none, false));
    TEST_ASSERT_EQUAL_UINT16(1000u, bms_derate_step(&d, 40u, true, FULL, COOL, none, true));
    TEST_ASSERT_EQUAL_UINT16(0u, bms_derate_step(&d, 60u, true, FULL, COOL, none, false));
}

/* Before any measurement there is nothing to allow. */
static void test_with_no_measurement_yet_the_limit_is_zero(void)
{
    TEST_ASSERT_EQUAL_UINT16(0u, bms_derate_step(&d, 0u, false, FULL, COOL, none, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_full_cool_pack_gets_the_full_limit);
    RUN_TEST(test_the_voltage_taper_runs_from_100_percent_at_3v0_to_20_percent_at_2v7);
    RUN_TEST(test_the_temperature_taper_runs_from_100_percent_at_50c_to_20_percent_at_57c);
    RUN_TEST(test_the_lower_taper_wins);
    RUN_TEST(test_the_voltage_taper_adds_the_sag_back);
    RUN_TEST(test_with_pack_current_invalid_the_taper_uses_the_raw_voltage);
    RUN_TEST(test_the_limit_falls_at_200_percent_a_second);
    RUN_TEST(test_the_limit_rises_at_10_percent_a_second);
    RUN_TEST(test_with_no_measurement_the_limit_holds);
    RUN_TEST(test_the_limit_is_zero_until_bms_ok_is_up);
    RUN_TEST(test_with_no_measurement_yet_the_limit_is_zero);
    return UNITY_END();
}
