/*
 * Host tests for the BMS's Control step as one function
 * (App/bms/logic/Inc/bms_control.h), issue 63.
 *
 * Seam 1 of spec #36: the test hands the step its inputs (the Sweep, the
 * digital inputs, the time) and judges only what comes out: the state, the
 * BMS_OK level, BMS_Status as bytes, the Events.
 *
 * Spec #36, "BMS image", the BMS fault table; ADR 0007:
 * - Over-voltage at >= 4.195 V after 3 bad Sweeps in a row (EV.8.4.2,
 *   "considering measurement accuracy"); under-voltage at <= 2.505 V raw
 *   loaded voltage after 10; over-temperature at a reading <= 1526 mV
 *   (58 C) after 25 (EV.8.5.2); under-temperature (>= 2146 mV) after 25,
 *   in Charge mode only; a missing or invalid measurement and chain comms
 *   loss after 5 (EV.8.3.4: the BMS must open the Shutdown Circuit when a
 *   measurement is missing or invalid).
 * - FAULT ends by itself after 50 clean Sweeps (1 s). BMS_OK first rises
 *   after the chain is awake and 5 clean Sweeps. A chain silent for 2 s
 *   in STARTUP is FAULT with "measurement missing or invalid".
 * - The Low-voltage limit (3.0 V) is a status bit, never a fault.
 * - Active and seen bits; overcurrent reserved and never set.
 * - No decision from a received frame; a Control bus failure never takes
 *   BMS_OK low (ADR 0005).
 */
#include <string.h>

#include "bms_control.h"
#include "bq_sweep.h"
#include "can_control.h"
#include "e2e_p11.h"
#include "event_codes.h"
#include "unity.h"

#define STEP_MS 20u

#define CELL_OK_MV 3700u
#define TEMP_OK_MV 1900u /* about 25 C */

static bms_control_t bc;
static bms_control_config_t config;
static bms_control_in_t in;
static bms_control_out_t out;
static bq_sweep_t sweep;

static void good_sweep(void)
{
    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        sweep.cell_mv[i] = CELL_OK_MV;
        sweep.temp_mv[i] = TEMP_OK_MV;
    }
    sweep.devices_read = 0xFFFFu;
}

void setUp(void)
{
    memset(&config, 0, sizeof config);
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    good_sweep();
    in.now_ms = 1000u;
    in.startup_ok = true;
    in.can_running = true;
    in.swept = true;
    in.sweep = &sweep;
    bms_control_init(&bc, &config);
}

void tearDown(void) {}

/* One 20 ms step. */
static void step(void)
{
    bms_control_step(&bc, &in, &out);
    in.now_ms += STEP_MS;
}

static void steps(unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        step();
    }
}

static struct can_control_bms_status_t status(void)
{
    struct can_control_bms_status_t s;
    TEST_ASSERT_TRUE(out.send_status);
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(&s, out.status, sizeof out.status));
    return s;
}

/* Power-up: BMS_OK rises only after the chain is awake and 5 clean Sweeps
   in a row (spec #36). */
static void test_bms_ok_rises_on_the_fifth_clean_sweep_and_not_the_fourth(void)
{
    steps(BMS_POWERUP_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_EQUAL_UINT8(0u, status().bms_ok);

    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, status().state);
    TEST_ASSERT_EQUAL_UINT8(1u, status().bms_ok);
    TEST_ASSERT_FALSE(out.fault_active);
}

static void power_up(void)
{
    steps(BMS_POWERUP_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* With the bad value already in this step's Sweep, n - 1 steps keep BMS_OK
   high and the nth takes it low, with fault's active and seen bits. */
static void assert_raised_on_sweep(unsigned n, bms_fault_t fault)
{
    steps(n - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(0u, out.active);

    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_TRUE(out.fault_active);
    TEST_ASSERT_EQUAL_HEX16(1u << fault, out.active);
    TEST_ASSERT_EQUAL_HEX16(1u << fault, out.seen);
    TEST_ASSERT_EQUAL_UINT8(0u, status().bms_ok);
}

/* The Event of the fault raised: the fault, the state it left the BMS in, which cell
   (kind 1) or sensor (kind 2), 1 to 128, and its reading. */
static void assert_fault_event(bms_fault_t fault, uint8_t kind, uint16_t index, uint16_t mv)
{
    TEST_ASSERT_EQUAL_size_t(1u, out.event_count);
    TEST_ASSERT_EQUAL_UINT16(EVENT_BMS_FAULT, out.events[0].code);
    const uint8_t *d = out.events[0].data;
    TEST_ASSERT_EQUAL_UINT8(fault, d[0]);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, d[1]);
    TEST_ASSERT_EQUAL_UINT8(kind, d[2]);
    TEST_ASSERT_EQUAL_UINT16(index, (uint16_t)(d[4] | (d[5] << 8)));
    TEST_ASSERT_EQUAL_UINT16(mv, (uint16_t)(d[6] | (d[7] << 8)));
}

/* EV.8.4.2: over-voltage at 4.195 V, the 4.2 V limit less the
   measurement accuracy, after 3 bad Sweeps (100 ms budget). */
static void test_over_voltage_is_raised_on_the_third_sweep_at_4195_mv(void)
{
    power_up();
    sweep.cell_mv[36] = 4195u; /* device 5, cell 5 */
    assert_raised_on_sweep(BMS_OV_SWEEPS, BMS_FAULT_OVER_VOLTAGE);
    assert_fault_event(BMS_FAULT_OVER_VOLTAGE, EVENT_BMS_CELL, 37u, 4195u);
    TEST_ASSERT_EQUAL_UINT8(1u, status().over_voltage_active);
    TEST_ASSERT_EQUAL_UINT8(1u, status().over_voltage_seen);
}

static void test_a_cell_at_4194_mv_is_not_over_voltage(void)
{
    power_up();
    sweep.cell_mv[0] = 4194u;
    steps(200u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);
}

/* Under-voltage at 2.505 V on the raw loaded voltage after 10 Sweeps
   (250 ms budget; EV.8.4.2 with the measurement accuracy). */
static void test_under_voltage_is_raised_on_the_tenth_sweep_at_2505_mv(void)
{
    power_up();
    sweep.cell_mv[127] = 2505u; /* device 16, cell 8 */
    sweep.cell_mv[3] = 2700u;   /* low, but not the lowest */
    assert_raised_on_sweep(BMS_UV_SWEEPS, BMS_FAULT_UNDER_VOLTAGE);
    assert_fault_event(BMS_FAULT_UNDER_VOLTAGE, EVENT_BMS_CELL, 128u, 2505u);
    TEST_ASSERT_EQUAL_UINT8(1u, status().under_voltage_active);
}

static void test_a_cell_at_2506_mv_is_not_under_voltage(void)
{
    power_up();
    sweep.cell_mv[5] = 2506u;
    steps(200u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* Over-temperature at 58 C (a reading at or below 1526 mV) after 25
   Sweeps (550 ms budget; EV.8.5.2 with the measurement accuracy). */
static void test_over_temperature_is_raised_on_the_25th_sweep_at_1526_mv(void)
{
    power_up();
    sweep.temp_mv[8] = 1526u; /* device 2, sensor 1 */
    assert_raised_on_sweep(BMS_TEMP_SWEEPS, BMS_FAULT_OVER_TEMP);
    assert_fault_event(BMS_FAULT_OVER_TEMP, EVENT_BMS_SENSOR, 9u, 1526u);
    TEST_ASSERT_EQUAL_UINT8(1u, status().over_temp_active);
}

static void test_a_reading_of_1527_mv_is_not_over_temperature(void)
{
    power_up();
    sweep.temp_mv[8] = 1527u;
    steps(200u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* Under-temperature (2 C, a reading at or above 2146 mV) is a fault in
   Charge mode only. */
static void test_under_temperature_is_raised_on_the_25th_sweep_in_charge_mode(void)
{
    in.charge_mode = true;
    steps(BMS_POWERUP_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);

    sweep.temp_mv[20] = 2146u;
    steps(BMS_TEMP_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_UNDER_TEMP, out.active);
    assert_fault_event(BMS_FAULT_UNDER_TEMP, EVENT_BMS_SENSOR, 21u, 2146u);
}

static void test_under_temperature_is_no_fault_out_of_charge_mode(void)
{
    power_up();
    sweep.temp_mv[20] = 2300u;
    steps(200u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);
}

/* EV.8.3.4: a missing measurement (a device not read) after 5 Sweeps. */
static void test_a_device_not_read_is_the_measurement_fault_on_the_fifth_sweep(void)
{
    power_up();
    sweep.devices_read = (uint16_t)~(1u << 3); /* device 4 */
    memset(&sweep.cell_mv[24], 0, 8u * sizeof sweep.cell_mv[0]);
    memset(&sweep.temp_mv[24], 0, 8u * sizeof sweep.temp_mv[0]);
    assert_raised_on_sweep(BMS_MEAS_SWEEPS, BMS_FAULT_MEASUREMENT);
    assert_fault_event(BMS_FAULT_MEASUREMENT, EVENT_BMS_CELL, 25u, 0u);
    TEST_ASSERT_EQUAL_UINT8(1u, status().measurement_active);
    /* The missing cells read 0 mV, but are not under-voltage. */
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen & (1u << BMS_FAULT_UNDER_VOLTAGE));
}

/* EV.8.3.4: an invalid measurement, a sensor outside 1300-2440 mV. */
static void test_a_sensor_above_its_range_is_the_measurement_fault(void)
{
    power_up();
    sweep.temp_mv[100] = 2441u;
    assert_raised_on_sweep(BMS_MEAS_SWEEPS, BMS_FAULT_MEASUREMENT);
    assert_fault_event(BMS_FAULT_MEASUREMENT, EVENT_BMS_SENSOR, 101u, 2441u);
}

static void test_a_sensor_below_its_range_is_the_measurement_fault_not_over_temperature(void)
{
    power_up();
    sweep.temp_mv[0] = 1299u;
    assert_raised_on_sweep(BMS_MEAS_SWEEPS, BMS_FAULT_MEASUREMENT);
    assert_fault_event(BMS_FAULT_MEASUREMENT, EVENT_BMS_SENSOR, 1u, 1299u);
}

static void test_a_cell_reading_0_mv_is_the_measurement_fault(void)
{
    power_up();
    sweep.cell_mv[64] = 0u;
    assert_raised_on_sweep(BMS_MEAS_SWEEPS, BMS_FAULT_MEASUREMENT);
    assert_fault_event(BMS_FAULT_MEASUREMENT, EVENT_BMS_CELL, 65u, 0u);
}

/* Chain comms loss after STARTUP: a step with no Sweep is a missing
   measurement, after 5. */
static void test_steps_with_no_sweep_after_startup_are_chain_comms_loss(void)
{
    power_up();
    in.swept = false;
    assert_raised_on_sweep(BMS_MEAS_SWEEPS, BMS_FAULT_MEASUREMENT);
    assert_fault_event(BMS_FAULT_MEASUREMENT, EVENT_BMS_NOWHERE, 0u, 0u);
}

/* A device that answers only every other Sweep cannot hold an
   over-voltage off: its missing Sweeps neither count nor clear it. */
static void test_an_intermittent_device_does_not_hold_off_over_voltage(void)
{
    power_up();
    for (unsigned i = 0; i < 2u * BMS_OV_SWEEPS - 1u; i++) {
        sweep.devices_read = (i % 2u == 0u) ? 0xFFFFu : 0xFFFEu;
        sweep.cell_mv[2] = (i % 2u == 0u) ? 4250u : 0u;
        step();
    }
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_OVER_VOLTAGE,
                            out.active & (1u << BMS_FAULT_OVER_VOLTAGE));
}

/* FAULT ends by itself after exactly 1 s, 50 clean Sweeps (ADR 0007);
   the active bit clears and the seen bit stays. */
static void test_fault_recovers_after_exactly_50_clean_sweeps(void)
{
    power_up();
    sweep.cell_mv[36] = 4300u;
    steps(BMS_OV_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);

    sweep.cell_mv[36] = CELL_OK_MV;
    steps(BMS_RECOVER_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);

    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(0u, out.active);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_OVER_VOLTAGE, out.seen);
    TEST_ASSERT_EQUAL_UINT8(0u, status().over_voltage_active);
    TEST_ASSERT_EQUAL_UINT8(1u, status().over_voltage_seen);
    TEST_ASSERT_EQUAL_size_t(0u, out.event_count);
}

/* Every value must be in range for the whole second: one bad Sweep of
   another kind starts it again, though it raises nothing. */
static void test_any_bad_sweep_starts_the_clean_second_again(void)
{
    power_up();
    sweep.cell_mv[36] = 4300u;
    steps(BMS_OV_SWEEPS);
    sweep.cell_mv[36] = CELL_OK_MV;
    steps(30u);
    sweep.cell_mv[7] = 2400u; /* one under-voltage Sweep */
    step();
    sweep.cell_mv[7] = CELL_OK_MV;
    steps(BMS_RECOVER_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_OVER_VOLTAGE, out.seen);
}

/* A value that stays bad keeps the fault on, however long. */
static void test_a_fault_lasts_while_its_cause_does(void)
{
    power_up();
    sweep.temp_mv[3] = 1400u;
    steps(500u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
}

/* Power-up: a step with no Sweep before BMS_OK has risen starts the 5
   again. */
static void test_a_step_with_no_sweep_in_startup_starts_the_five_again(void)
{
    steps(3u);
    in.swept = false; /* the chain still waking on this step */
    step();
    in.swept = true;
    steps(BMS_POWERUP_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* Power-up: while the chain wakes there is no Sweep, and BMS_OK stays
   low; under 2 s that is no fault. */
static void test_a_waking_chain_keeps_startup_without_a_fault(void)
{
    in.swept = false;
    steps(BMS_CHAIN_SILENT_MS / STEP_MS); /* 0 .. 1980 ms */
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);

    in.swept = true;
    steps(BMS_POWERUP_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* A chain that has not answered for 2 s moves STARTUP to FAULT with
   "measurement missing or invalid" (spec #36). */
static void test_a_chain_silent_for_2_s_in_startup_is_the_measurement_fault(void)
{
    in.swept = false;
    steps(BMS_CHAIN_SILENT_MS / STEP_MS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
    step(); /* 2000 ms after the first step */
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_MEASUREMENT, out.active);
    assert_fault_event(BMS_FAULT_MEASUREMENT, EVENT_BMS_NOWHERE, 0u, 0u);

    /* Once the chain answers, 1 s clean ends it. */
    in.swept = true;
    steps(BMS_RECOVER_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* The 2 s is measured by subtraction, across the 32-bit wrap. */
static void test_the_silent_chain_time_survives_the_wrap(void)
{
    in.now_ms = 0xFFFFFFFFu - 1000u;
    in.swept = false;
    steps(BMS_CHAIN_SILENT_MS / STEP_MS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
}

/* A start-up that is blocked (a task or queue missing) never raises
   BMS_OK, and blinks the heartbeat. */
static void test_a_blocked_startup_never_raises_bms_ok(void)
{
    in.startup_ok = false;
    steps(200u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_TRUE(out.fault_active);
}

/* The Low-voltage limit (3.0 V) sets a status bit and never takes BMS_OK
   low. */
static void test_the_low_voltage_limit_is_a_status_bit_and_no_fault(void)
{
    power_up();
    sweep.cell_mv[50] = 2999u;
    steps(200u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_TRUE(out.low_voltage_limit);
    TEST_ASSERT_EQUAL_UINT8(1u, status().low_voltage_limit);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);

    sweep.cell_mv[50] = 3000u;
    step();
    TEST_ASSERT_FALSE(out.low_voltage_limit);
}

/* The BMS makes no decision from a received frame, and a Control bus
   failure never takes BMS_OK low (ADR 0005): the bus only fills bits. */
static void test_a_control_bus_failure_never_takes_bms_ok_low(void)
{
    power_up();
    in.bus_off_latched = true;
    in.error_passive = true;
    in.rx_overflowed = true;
    steps(200u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_UINT8(1u, status().bus_off_latched);
    TEST_ASSERT_EQUAL_UINT8(1u, status().error_passive);
    TEST_ASSERT_EQUAL_UINT8(1u, status().receive_overflow);
}

/* Overcurrent is reserved and never set, whatever the pack does. */
static void test_overcurrent_is_never_set(void)
{
    power_up();
    sweep.cell_mv[0] = 4300u;
    sweep.cell_mv[1] = 2000u;
    sweep.temp_mv[2] = 1400u;
    steps(100u);
    TEST_ASSERT_EQUAL_UINT8(0u, status().overcurrent_active);
    TEST_ASSERT_EQUAL_UINT8(0u, status().overcurrent_seen);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen & (1u << BMS_FAULT_OVERCURRENT));
}

/* Each fault raised is one Event, on the step its own count runs out. */
static void test_each_fault_is_one_event_on_the_step_it_is_raised(void)
{
    power_up();
    sweep.cell_mv[0] = 4300u;   /* over-voltage, 3 Sweeps */
    sweep.temp_mv[0] = 1200u;   /* invalid: measurement, 5 Sweeps */
    steps(BMS_OV_SWEEPS);
    TEST_ASSERT_EQUAL_size_t(1u, out.event_count);
    TEST_ASSERT_EQUAL_UINT8(BMS_FAULT_OVER_VOLTAGE, out.events[0].data[0]);
    steps(BMS_MEAS_SWEEPS - BMS_OV_SWEEPS);
    TEST_ASSERT_EQUAL_size_t(1u, out.event_count);
    TEST_ASSERT_EQUAL_UINT8(BMS_FAULT_MEASUREMENT, out.events[0].data[0]);

    setUp();
    power_up();
    sweep.cell_mv[0] = 2000u;   /* under-voltage, 10 */
    sweep.temp_mv[0] = 1400u;   /* over-temperature, 25 */
    steps(BMS_UV_SWEEPS);
    TEST_ASSERT_EQUAL_size_t(1u, out.event_count);
    steps(BMS_TEMP_SWEEPS - BMS_UV_SWEEPS);
    TEST_ASSERT_EQUAL_size_t(1u, out.event_count);
    TEST_ASSERT_EQUAL_HEX16((1u << BMS_FAULT_UNDER_VOLTAGE) | (1u << BMS_FAULT_OVER_TEMP),
                            out.active);
}

/* The Held fault (#65 decides when): FAULT_HELD from the first step with
   the internal fault, and no recovery. */
static void test_the_held_fault_never_ends(void)
{
    config.held = true;
    bms_control_init(&bc, &config);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT_HELD, out.state);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_INTERNAL, out.active);
    TEST_ASSERT_EQUAL_size_t(1u, out.event_count);
    TEST_ASSERT_EQUAL_UINT8(BMS_FAULT_INTERNAL, out.events[0].data[0]);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT_HELD, out.events[0].data[1]);

    steps(500u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT_HELD, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_TRUE(out.fault_active);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_INTERNAL, out.active);
    TEST_ASSERT_EQUAL_UINT8(1u, status().internal_active);
    TEST_ASSERT_EQUAL_size_t(0u, out.event_count);
}

/* BMS_Status goes every step from the first, protected by E2E Profile 11
   (Data ID 0x0080), with the configuration bits. */
static void test_bms_status_goes_every_step_and_passes_its_e2e_check(void)
{
    config.params_defaulted = true;
    config.last_reset_not_power_on = true;
    bms_control_init(&bc, &config);
    in.sdc_closed = true;
    e2e_p11_rx_t rx = {0};

    for (unsigned i = 0; i < 20u; i++) {
        step();
        TEST_ASSERT_TRUE(out.send_status);
        TEST_ASSERT_EQUAL(E2E_P11_OK, e2e_p11_check(&rx, 0x0080u, out.status, sizeof out.status));
    }
    TEST_ASSERT_EQUAL_UINT8(1u, status().parameters_defaulted);
    TEST_ASSERT_EQUAL_UINT8(1u, status().last_reset_not_power_on);
    TEST_ASSERT_EQUAL_UINT8(0u, status().not_dual_bank);
    TEST_ASSERT_EQUAL_UINT8(1u, status().shutdown_circuit_closed);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bms_ok_rises_on_the_fifth_clean_sweep_and_not_the_fourth);
    RUN_TEST(test_over_voltage_is_raised_on_the_third_sweep_at_4195_mv);
    RUN_TEST(test_a_cell_at_4194_mv_is_not_over_voltage);
    RUN_TEST(test_under_voltage_is_raised_on_the_tenth_sweep_at_2505_mv);
    RUN_TEST(test_a_cell_at_2506_mv_is_not_under_voltage);
    RUN_TEST(test_over_temperature_is_raised_on_the_25th_sweep_at_1526_mv);
    RUN_TEST(test_a_reading_of_1527_mv_is_not_over_temperature);
    RUN_TEST(test_under_temperature_is_raised_on_the_25th_sweep_in_charge_mode);
    RUN_TEST(test_under_temperature_is_no_fault_out_of_charge_mode);
    RUN_TEST(test_a_device_not_read_is_the_measurement_fault_on_the_fifth_sweep);
    RUN_TEST(test_a_sensor_above_its_range_is_the_measurement_fault);
    RUN_TEST(test_a_sensor_below_its_range_is_the_measurement_fault_not_over_temperature);
    RUN_TEST(test_a_cell_reading_0_mv_is_the_measurement_fault);
    RUN_TEST(test_steps_with_no_sweep_after_startup_are_chain_comms_loss);
    RUN_TEST(test_an_intermittent_device_does_not_hold_off_over_voltage);
    RUN_TEST(test_fault_recovers_after_exactly_50_clean_sweeps);
    RUN_TEST(test_any_bad_sweep_starts_the_clean_second_again);
    RUN_TEST(test_a_fault_lasts_while_its_cause_does);
    RUN_TEST(test_a_step_with_no_sweep_in_startup_starts_the_five_again);
    RUN_TEST(test_a_waking_chain_keeps_startup_without_a_fault);
    RUN_TEST(test_a_chain_silent_for_2_s_in_startup_is_the_measurement_fault);
    RUN_TEST(test_the_silent_chain_time_survives_the_wrap);
    RUN_TEST(test_a_blocked_startup_never_raises_bms_ok);
    RUN_TEST(test_the_low_voltage_limit_is_a_status_bit_and_no_fault);
    RUN_TEST(test_a_control_bus_failure_never_takes_bms_ok_low);
    RUN_TEST(test_overcurrent_is_never_set);
    RUN_TEST(test_each_fault_is_one_event_on_the_step_it_is_raised);
    RUN_TEST(test_the_held_fault_never_ends);
    RUN_TEST(test_bms_status_goes_every_step_and_passes_its_e2e_check);
    return UNITY_END();
}
