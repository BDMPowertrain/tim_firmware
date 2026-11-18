/*
 * The Power limiter (App/vcu/logic/Inc/vcu_power_limiter.h) in a closed
 * loop against a plant model, issue 60, ADR 0017.
 *
 * Rules:
 * - EV.3.1.3: the torque commanded never exceeds the driver's request.
 *   Asserted here on every control step.
 * - EV.3.3.1, EV.3.4.1: pack power at most 80 kW, judged over two windows
 *   (100 ms continuously above, 500 ms moving average above). Judged by
 *   tools/power_rule_windows.py, the one check bench scripts and car logs
 *   use too: each scenario writes its trace, power_limiter_<scenario>.csv,
 *   and CTest runs the check on every one (test_power_rule_windows_plant).
 *
 * The plant, every 1 ms (the control step runs every 10 ms):
 * - Pack: 128 cells in series at 4.0 V (512 V open circuit), 0.2 ohm of
 *   resistance, and a sag that builds over 2 s (0.1 ohm, a polarisation
 *   RC), so the terminal voltage falls under load and recovers after.
 *   Pack power is terminal voltage x current: what the Energy Meter sees.
 * - BMS: samples pack current and voltage every 20 ms (one sweep) and
 *   sends BMS_Power, which reaches the step 5 ms later; its current reads
 *   1% low, the direction that hides power from the trim.
 * - Inverter and motor: the torque follows the command with a 5 ms lag,
 *   from the next step's frame; electrical power is torque x speed / the
 *   true efficiency, which the scenario sets apart from the limiter's 0.85.
 * - Speed: held by a dynamometer, or a 300 kg car on 0.23 m wheels through
 *   a 3.5:1 gear with air drag, from standstill.
 * The driver's request is the pedal x 230 N.m, after the gates.
 *
 * Scenarios: a speed sweep (a launch from standstill to 6000 rpm), pedal
 * stamps at four held speeds, the same with an efficiency of 0.75 where
 * the limiter assumes 0.85, and a Discharge limit that binds while the
 * pack sags and recovers (the sag-recovery loop).
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "vcu_power_limiter.h"

#define PLANT_MS      1u
#define STEP_MS       10u
#define SWEEP_MS      20u  /* BMS_Power */
#define FRAME_LAG_MS  5u   /* BMS_Power from sample to the step's mailbox */
#define MAX_REQUEST   2300 /* 230 N.m, 0.1 N.m */

#define CELLS         128.0
#define OCV_V         (CELLS * 4.0)
#define R0_OHM        0.2
#define R1_OHM        0.1
#define SAG_TAU_S     2.0
#define CURRENT_GAIN  0.99 /* the BMS reads 1% low */
#define TORQUE_TAU_S  0.005

#define CAR_KG        300.0
#define WHEEL_M       0.23
#define GEAR          3.5
#define DRAG_N_PER_V2 0.72 /* 0.5 x 1.2 kg/m3 x 1.2 m2 Cd.A */
#define PI            3.14159265358979

typedef struct {
    const char *name;
    double      efficiency;         /* the plant's true one */
    double      discharge_limit_a;  /* what the BMS sends */
    bool        dyno;               /* speed held, not a car */
} scenario_t;

typedef struct {
    /* The plant. */
    double  sag_v;        /* across the polarisation RC */
    double  current_a;
    double  volts;        /* terminal */
    double  torque_nm;    /* at the shaft */
    double  rpm;
    double  car_mps;
    /* The bus between them. */
    int16_t command;      /* the last torque the step sent, 0.1 N.m */
    bool    frame_due;
    uint32_t frame_at_ms;
    vcu_power_limiter_in_t pending; /* the frame on its way */
    /* The step. */
    vcu_power_limiter_t pl;
    vcu_power_limiter_in_t    in;
    vcu_power_limiter_out_t   out;
    double              pedal; /* 0 to 1 */
    /* What the test judges. */
    FILE   *trace;
    double  peak_power_w;
} sim_t;

static sim_t sim;
static scenario_t sc;

static void sim_start(const scenario_t *scenario, double rpm)
{
    memset(&sim, 0, sizeof(sim));
    sc = *scenario;
    sim.volts = OCV_V;
    sim.rpm = rpm;
    sim.car_mps = rpm / 60.0 * 2.0 * PI / GEAR * WHEEL_M;
    vcu_power_limiter_init(&sim.pl);
    sim.in.power_valid = true;

    char path[96];
    (void)snprintf(path, sizeof(path), "power_limiter_%s.csv", sc.name);
    sim.trace = fopen(path, "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(sim.trace, path);
    fprintf(sim.trace, "time_ms,pack_power_w,pack_v,pack_a,rpm,request_dnm,torque_dnm,"
                       "trim_ppm,lower\n");
}

static void sim_end(void)
{
    (void)fclose(sim.trace);
}

/* One 1 ms tick at time t. */
static void tick(uint32_t t)
{
    /* The step, every 10 ms: the frame that has arrived, the speed it
       last heard, the request after the gates. */
    if (t % STEP_MS == 0u) {
        if (sim.frame_due && (uint32_t)(t - sim.frame_at_ms) >= FRAME_LAG_MS) {
            sim.in.power_frames = sim.pending.power_frames;
            sim.in.pack_current_da = sim.pending.pack_current_da;
            sim.in.pack_voltage_dv = sim.pending.pack_voltage_dv;
            sim.in.discharge_limit_da = sim.pending.discharge_limit_da;
            sim.frame_due = false;
        }
        sim.in.now_ms = t;
        sim.in.motor_speed_rpm = (int16_t)sim.rpm;
        sim.in.request = (int16_t)(sim.pedal * MAX_REQUEST);
        vcu_power_limiter_step(&sim.pl, &sim.in, &sim.out);
        /* EV.3.1.3, on every step. */
        TEST_ASSERT_TRUE(sim.out.torque <= sim.in.request);
        TEST_ASSERT_TRUE(sim.out.torque >= 0);
        sim.command = sim.out.torque;
    }

    /* The BMS sweep, every 20 ms: one sample of current and voltage. */
    if (t % SWEEP_MS == 0u) {
        sim.pending.power_frames++;
        sim.pending.pack_current_da = (int16_t)(sim.current_a * CURRENT_GAIN * 10.0);
        sim.pending.pack_voltage_dv = (uint16_t)(sim.volts * 10.0);
        sim.pending.discharge_limit_da = (uint16_t)(sc.discharge_limit_a * 10.0);
        sim.frame_due = true;
        sim.frame_at_ms = t;
    }

    /* The inverter: torque follows the command. */
    const double dt = PLANT_MS / 1000.0;
    sim.torque_nm += ((double)sim.command / 10.0 - sim.torque_nm) * dt / TORQUE_TAU_S;
    const double omega = sim.rpm * 2.0 * PI / 60.0;
    const double power_w = sim.torque_nm * omega / sc.efficiency;

    /* The pack: the current that delivers that power at the terminal
       voltage it makes, a few rounds of P = (OCV - sag - I R0) I. */
    double amps = sim.current_a;
    for (int i = 0; i < 4; i++) {
        const double v = OCV_V - sim.sag_v - amps * R0_OHM;
        amps = power_w / v;
    }
    sim.current_a = amps;
    sim.volts = OCV_V - sim.sag_v - amps * R0_OHM;
    sim.sag_v += (amps * R1_OHM - sim.sag_v) * dt / SAG_TAU_S;

    /* The speed. */
    if (!sc.dyno) {
        const double force = sim.torque_nm * GEAR / WHEEL_M - DRAG_N_PER_V2 * sim.car_mps * sim.car_mps;
        sim.car_mps += force / CAR_KG * dt;
        if (sim.car_mps < 0.0) {
            sim.car_mps = 0.0;
        }
        sim.rpm = sim.car_mps / WHEEL_M * GEAR * 60.0 / (2.0 * PI);
    }

    const double pack_w = sim.volts * sim.current_a;
    if (pack_w > sim.peak_power_w) {
        sim.peak_power_w = pack_w;
    }
    fprintf(sim.trace, "%lu,%.1f,%.2f,%.2f,%.0f,%d,%d,%ld,%u\n", (unsigned long)t, pack_w,
            sim.volts, sim.current_a, sim.rpm, sim.in.request, sim.out.torque,
            (long)sim.out.trim_ppm, (unsigned)sim.out.lower);
}

static uint32_t now;

static void run_ms(uint32_t ms)
{
    for (uint32_t end = now + ms; now < end; now += PLANT_MS) {
        tick(now);
    }
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                          */
/* ------------------------------------------------------------------ */

void setUp(void) { now = 0u; }
void tearDown(void) {}

/* A launch from standstill, full pedal until 6000 rpm: the speed sweeps
   through the whole range, the ceiling falls as 1/speed. */
static void test_a_launch_sweeps_the_speed_and_holds_the_rule_windows(void)
{
    const scenario_t s = { "speed_sweep", 0.92, 300.0, false };
    sim_start(&s, 0.0);
    run_ms(200u);   /* BMS_Power VALID, pedal up */
    sim.pedal = 1.0;
    while (sim.rpm < 6000.0 && now < 15000u) {
        run_ms(10u);
    }
    TEST_ASSERT_TRUE_MESSAGE(sim.rpm >= 6000.0, "the car never reached 6000 rpm");
    sim.pedal = 0.0;
    run_ms(500u);
    sim_end();
    /* The ceiling bound: the car was given up to its 72 kW less the
       efficiency the limiter does not know (0.85 / 0.92 of it). */
    TEST_ASSERT_TRUE(sim.peak_power_w > 60000.0);
    TEST_ASSERT_TRUE(sim.peak_power_w < 80000.0);
}

/* Full pedal stamped on and off at held speeds: each stamp jumps to the
   ceiling within the inverter's lag. */
static void stamps(const scenario_t *s)
{
    static const double speeds[] = { 1500.0, 3000.0, 4500.0, 6000.0 };
    sim_start(s, speeds[0]);
    run_ms(200u);
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        sim.rpm = speeds[i];
        for (int k = 0; k < 3; k++) {
            sim.pedal = 1.0;
            run_ms(400u);
            sim.pedal = 0.0;
            run_ms(300u);
        }
    }
    sim_end();
}

static void test_pedal_stamps_at_held_speeds_hold_the_rule_windows(void)
{
    const scenario_t s = { "pedal_stamps", 0.92, 300.0, true };
    stamps(&s);
    /* The trim learnt the efficiency is better than 0.85 and kept it. */
    TEST_ASSERT_EQUAL_INT32(0, sim.out.trim_ppm);
}

/* The efficiency constant wrong in the bad direction: the motor and
   inverter lose 25%, the limiter thinks 15%, so the feed-forward alone
   asks for 72 x 0.85 / 0.75 = 81.6 kW. The trim must find the 12% and
   hold it between pulls. */
static void test_a_wrong_efficiency_constant_is_caught_by_the_trim(void)
{
    const scenario_t s = { "wrong_efficiency", 0.75, 300.0, true };
    stamps(&s);
    TEST_ASSERT_TRUE(sim.out.trim_ppm < -100000);
    TEST_ASSERT_TRUE(sim.out.trim_ppm > VCU_TRIM_MIN_PPM);
    TEST_ASSERT_TRUE(sim.peak_power_w < 80000.0);
}

/* A Discharge limit that binds: 140 A at a pack that sags under it. The
   ceiling, 140 A x the sagged voltage, falls as the pack sags and rises
   as it recovers; the limiter must follow it without cycling between full
   torque and zero, and hold the current at the limit. */
static void test_the_sag_recovery_loop_settles_at_the_discharge_limit(void)
{
    const scenario_t s = { "sag_recovery", 0.92, 140.0, true };
    sim_start(&s, 3000.0);
    run_ms(200u);
    double min_torque = 1e9;
    double max_torque = 0.0;
    double max_amps = 0.0;
    for (int pull = 0; pull < 3; pull++) {
        sim.pedal = 1.0;
        run_ms(1000u); /* settle */
        TEST_ASSERT_EQUAL_UINT8(VCU_CEILING_DISCHARGE, sim.out.lower);
        for (uint32_t ms = 0; ms < 2000u; ms += STEP_MS) {
            run_ms(STEP_MS);
            const double nm = (double)sim.out.torque / 10.0;
            min_torque = nm < min_torque ? nm : min_torque;
            max_torque = nm > max_torque ? nm : max_torque;
            max_amps = sim.current_a > max_amps ? sim.current_a : max_amps;
        }
        sim.pedal = 0.0;
        run_ms(1500u); /* the pack recovers */
    }
    sim_end();
    /* No cycling: the torque only drifts with the sag. */
    TEST_ASSERT_TRUE(max_torque - min_torque < 0.1 * max_torque);
    /* The current held at the limit, the trim's error only (the BMS
       reads 1% low). */
    TEST_ASSERT_TRUE(max_amps < 140.0 * 1.02);
    TEST_ASSERT_TRUE(max_amps > 140.0 * 0.85);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_launch_sweeps_the_speed_and_holds_the_rule_windows);
    RUN_TEST(test_pedal_stamps_at_held_speeds_hold_the_rule_windows);
    RUN_TEST(test_a_wrong_efficiency_constant_is_caught_by_the_trim);
    RUN_TEST(test_the_sag_recovery_loop_settles_at_the_discharge_limit);
    return UNITY_END();
}
