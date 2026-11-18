/*
 * Host tests for the Power limiter (App/vcu/logic/Inc/vcu_power_limiter.h),
 * issue 60, ADR 0017.
 *
 * Rules:
 * - EV.3.1.3: the torque commanded never exceeds the driver's request; a
 *   limiter only lowers it.
 * - EV.3.3.1: pack power at most 80 kW; the Power ceiling holds 72 kW, or
 *   less when the BMS's Discharge limit allows less.
 *
 * Expected values are worked by hand from the formula in the issue:
 * torque ceiling = Power ceiling x 0.85 / (rpm x 2 pi / 60), rounded down
 * to 0.1 N.m. The closed loop against a plant is
 * test_vcu_power_limiter_plant.c.
 */
#include <string.h>

#include "unity.h"
#include "vcu_power_limiter.h"

static vcu_power_limiter_t pl;
static vcu_limiter_in_t in;
static vcu_limiter_out_t out;

void setUp(void)
{
    vcu_power_limiter_init(&pl);
    in = (vcu_limiter_in_t){
        .now_ms = 1000u,
        .request = 3000,             /* 300 N.m: more than any ceiling below */
        .motor_speed_rpm = 3000,     /* 314.16 rad/s */
        .power_valid = true,
        .power_frames = 0u,
        .pack_current_da = 0,
        .pack_voltage_dv = 4000u,    /* 400 V */
        .discharge_limit_da = 2500u, /* 250 A: 100 kW, above the target */
    };
}

void tearDown(void) {}

static void step(void)
{
    vcu_power_limiter_step(&pl, &in, &out);
    in.now_ms += 10u; /* the control step */
}

/* Steps binding until a frame could have been measured while binding. */
static void bind_until_settled(void)
{
    for (uint32_t ms = 0; ms < VCU_TRIM_SETTLE_MS; ms += 10u) {
        step();
    }
}

/* A new BMS_Power frame reading this many watts of pack power at 400 V. */
static void frame_with_power(uint32_t watts)
{
    in.power_frames++;
    in.pack_current_da = (int16_t)(watts / 40u); /* W / 400 V, in 0.1 A */
    step();
}

/* The trim learnt all the way up: one frame with no pack power while the
   limiter binds asks for +25% and stops at 0. */
static void learn_trim_to_zero(void)
{
    bind_until_settled();
    frame_with_power(0u);
    TEST_ASSERT_EQUAL_INT32(0, out.trim_ppm);
}

/* ------------------------------------------------------------------ */
/* The ceilings                                                       */
/* ------------------------------------------------------------------ */

/* EV.3.3.1. From reset the trim is at its bound, -15%: 61.2 kW, x 0.85,
   / 314.16 rad/s = 165.58 N.m. */
static void test_from_reset_the_ceiling_is_the_72_kw_target_less_15_percent(void)
{
    step();
    TEST_ASSERT_EQUAL_UINT32(72000u, out.power_ceiling_w);
    TEST_ASSERT_EQUAL_INT32(-150000, out.trim_ppm);
    TEST_ASSERT_EQUAL_INT16(1655, out.torque_ceiling);
    TEST_ASSERT_EQUAL_INT16(1655, out.torque);
    TEST_ASSERT_TRUE(out.active);
    TEST_ASSERT_EQUAL_UINT8(VCU_CEILING_TARGET, out.lower);
}

/* EV.3.3.1 with the BMS asking for less: 150 A x 400 V = 60 kW, under the
   target. Less 15%, 51 kW, x 0.85 / 314.16 rad/s = 137.99 N.m. */
static void test_the_discharge_limit_times_pack_voltage_is_the_ceiling_when_lower(void)
{
    in.discharge_limit_da = 1500u;
    step();
    TEST_ASSERT_EQUAL_UINT32(60000u, out.power_ceiling_w);
    TEST_ASSERT_EQUAL_UINT8(VCU_CEILING_DISCHARGE, out.lower);
    TEST_ASSERT_EQUAL_INT16(1379, out.torque);
}

/* 0 A until BMS_OK is up (can/control.dbc): no power, no torque. */
static void test_a_zero_discharge_limit_allows_no_torque(void)
{
    in.discharge_limit_da = 0u;
    step();
    TEST_ASSERT_EQUAL_UINT32(0u, out.power_ceiling_w);
    TEST_ASSERT_EQUAL_INT16(0, out.torque);
}

/* The speed is floored at about 100 rpm, and taken as its size. 10 A x
   400 V = 4 kW, less 15% = 3.4 kW, x 0.85 / 10.47 rad/s = 275.97 N.m;
   at 200 rpm half that. */
static void test_the_speed_is_floored_at_100_rpm_and_backwards_counts_as_forwards(void)
{
    static const int16_t floored[] = { 0, 50, -50, 100, -100 };
    in.discharge_limit_da = 100u;
    for (size_t i = 0; i < sizeof(floored) / sizeof(floored[0]); i++) {
        in.motor_speed_rpm = floored[i];
        step();
        TEST_ASSERT_EQUAL_INT16(2759, out.torque_ceiling);
    }
    in.motor_speed_rpm = 200;
    step();
    TEST_ASSERT_EQUAL_INT16(1379, out.torque_ceiling);
}

/* EV.3.1.3: whatever the speed, the request and the ceiling, the torque is
   the lower of the request and the ceiling, so never above the request;
   active exactly when it is below. */
static void test_the_torque_is_never_above_the_request(void)
{
    for (int16_t rpm = -2000; rpm <= 20000; rpm += 250) {
        for (int16_t request = 0; request <= 3000; request += 100) {
            in.motor_speed_rpm = rpm;
            in.request = request;
            step();
            TEST_ASSERT_TRUE(out.torque <= request);
            TEST_ASSERT_TRUE(out.torque <= out.torque_ceiling);
            TEST_ASSERT_TRUE(out.torque == request || out.torque == out.torque_ceiling);
            TEST_ASSERT_EQUAL(out.torque < request, out.active);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The trim (ADR 0017)                                                */
/* ------------------------------------------------------------------ */

/* Pack power under the ceiling while binding raises the trim, and it stops
   at 0: never above the feed-forward ceiling (EV.3.3.1). 72 kW x 0.85 /
   314.16 rad/s = 194.80 N.m. */
static void test_the_trim_rises_to_zero_and_never_above(void)
{
    learn_trim_to_zero();
    TEST_ASSERT_EQUAL_INT16(1948, out.torque);
    frame_with_power(0u);
    TEST_ASSERT_EQUAL_INT32(0, out.trim_ppm);
}

/* 79.2 kW is 10% over 72 kW: a quarter of that, -2.5%. 70.2 kW x 0.85 /
   314.16 rad/s = 189.93 N.m. */
static void test_pack_power_over_the_ceiling_lowers_the_trim_by_a_quarter_of_the_error(void)
{
    learn_trim_to_zero();
    frame_with_power(79200u);
    TEST_ASSERT_EQUAL_INT32(-25000, out.trim_ppm);
    TEST_ASSERT_EQUAL_INT16(1899, out.torque);
}

static void test_the_trim_stops_at_minus_15_percent(void)
{
    learn_trim_to_zero();
    for (int i = 0; i < 20; i++) {
        frame_with_power(144000u); /* twice the ceiling */
    }
    TEST_ASSERT_EQUAL_INT32(-150000, out.trim_ppm);
    TEST_ASSERT_EQUAL_INT16(1655, out.torque);
}

/* BMS_Power comes every 20 ms, the step every 10 ms: a step with no new
   frame leaves the trim alone, however far off the last frame was. */
static void test_the_trim_moves_only_on_a_new_frame(void)
{
    learn_trim_to_zero();
    frame_with_power(79200u);
    step();
    step();
    TEST_ASSERT_EQUAL_INT32(-25000, out.trim_ppm);
}

/* A request under the ceiling does not bind: a frame with no pack power
   then says nothing about the efficiency, and the trim stays. */
static void test_the_trim_moves_only_while_the_limiter_binds(void)
{
    in.request = 100; /* 10 N.m, far under 165 N.m */
    frame_with_power(0u);
    TEST_ASSERT_FALSE(out.active);
    TEST_ASSERT_EQUAL_INT16(100, out.torque);
    TEST_ASSERT_EQUAL_INT32(-150000, out.trim_ppm);
}

/* A pull's first frames were sampled before its torque rose: they read
   the pedal-up power, and would wind the trim up for nothing. Only a frame
   VCU_TRIM_SETTLE_MS into binding moves it. */
static void test_frames_in_the_first_50_ms_of_binding_leave_the_trim_alone(void)
{
    in.request = 0;
    step();
    in.request = 3000;
    for (uint32_t ms = 0u; ms < VCU_TRIM_SETTLE_MS; ms += 10u) {
        frame_with_power(0u);
        TEST_ASSERT_EQUAL_INT32(-150000, out.trim_ppm);
    }
    frame_with_power(0u);
    TEST_ASSERT_EQUAL_INT32(0, out.trim_ppm);
}

/* Lift off, coast with the pedal up, pull again: the second pull starts
   with what the first learnt. */
static void test_the_trim_is_kept_between_pulls(void)
{
    learn_trim_to_zero();
    frame_with_power(79200u);
    in.request = 0;
    for (int i = 0; i < 50; i++) {
        frame_with_power(0u);
    }
    in.request = 3000;
    frame_with_power(0u); /* a stale frame from the coast */
    TEST_ASSERT_EQUAL_INT32(-25000, out.trim_ppm);
    TEST_ASSERT_EQUAL_INT16(1899, out.torque);
}

/* ADR 0017: after BMS_Power has been invalid the trim starts at its
   bound, whatever it had learnt. */
static void test_the_trim_starts_at_minus_15_percent_after_bms_power_was_invalid(void)
{
    learn_trim_to_zero();
    in.power_valid = false;
    step();
    in.power_valid = true;
    step();
    TEST_ASSERT_EQUAL_INT32(-150000, out.trim_ppm);
    TEST_ASSERT_EQUAL_INT16(1655, out.torque);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_from_reset_the_ceiling_is_the_72_kw_target_less_15_percent);
    RUN_TEST(test_the_discharge_limit_times_pack_voltage_is_the_ceiling_when_lower);
    RUN_TEST(test_a_zero_discharge_limit_allows_no_torque);
    RUN_TEST(test_the_speed_is_floored_at_100_rpm_and_backwards_counts_as_forwards);
    RUN_TEST(test_the_torque_is_never_above_the_request);
    RUN_TEST(test_the_trim_rises_to_zero_and_never_above);
    RUN_TEST(test_pack_power_over_the_ceiling_lowers_the_trim_by_a_quarter_of_the_error);
    RUN_TEST(test_the_trim_stops_at_minus_15_percent);
    RUN_TEST(test_the_trim_moves_only_on_a_new_frame);
    RUN_TEST(test_the_trim_moves_only_while_the_limiter_binds);
    RUN_TEST(test_frames_in_the_first_50_ms_of_binding_leave_the_trim_alone);
    RUN_TEST(test_the_trim_is_kept_between_pulls);
    RUN_TEST(test_the_trim_starts_at_minus_15_percent_after_bms_power_was_invalid);
    return UNITY_END();
}
