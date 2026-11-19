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
 * - A cell under the Low-voltage limit (3.0 V) is a status bit, never a
 *   BMS fault.
 * - Active and seen bits; overcurrent reserved and never set.
 * - No decision from a received frame; a Control bus failure never takes
 *   BMS_OK low (ADR 0005).
 *
 * Issue 64, the second layers:
 * - The open-wire check runs once before BMS_OK first rises and then once
 *   per second, after the step's work; two failed checks in a row are the
 *   open sense wire fault (EV.8.3.4 b: "voltage sense Overcurrent
 *   Protection device(s) blown or tripped"; 2.5 s budget).
 * - NFAULT low, or a device reporting its own fault (the bridge's
 *   FAULT_SUMMARY), is the cell monitor fault.
 * - The BMS_OK read-back must equal the commanded level AND the NFAULT
 *   read-back: 5 Sweeps of mismatch are the BMS internal fault. The NFAULT
 *   read-back must agree with the bridge's fault register. BMS_OK low for
 *   500 ms with the Shutdown Circuit sense still energised raises the
 *   internal fault (EV.8.3.4 e, "a fault in the BMS").
 *
 * Issue 65 (ADR 0007, 0011):
 * - The Held fault after a watchdog, software, reset-pin or option-byte-
 *   load reset; a brown-out sets BOR and is a Power-on reset; with the core
 *   debug enable bit set any reset is a Power-on reset. Held, BMS_OK never
 *   rises and FAULT_HELD never ends.
 * - Three Overruns within one second are the BMS internal fault, ending
 *   after 1 s clean.
 *
 * Issue 66 (ADR 0018; spec #36, the Control bus table):
 * - The five-step send cycle: BMS_Status and BMS_Power every step, plus one
 *   of BMS_Limits, 0x202, 0x60D, BMS_PackVoltages, BMS_PackTemps.
 * - The Discharge limit in BMS_Power (0.1 A) and unscaled in 0x202 (whole
 *   amps); the charge limit 0 A in DRIVE; both 0 until BMS_OK is up.
 * - Pack voltage is the sum of the cell voltages of the same Sweep; pack
 *   current enters with a validity flag.
 * - The under-voltage fault stays on the raw loaded voltage.
 *
 * Issue 67 (ADR 0009, 0019): charge control through the step. The charge
 * logic itself is test_bms_charge.c's.
 * - Charge mode begins when the charger status frame is first heard and
 *   lasts until a power cycle; the state is CHARGE, and a silent charger
 *   never takes BMS_OK low.
 * - The request (0x1806E5F4) every 1 s in CHARGE only, in the 0x202 slot
 *   on one step in fifty; no request is ever sent outside CHARGE.
 * - Charger disobeys is a BMS fault. No charger flag is one.
 * - Charge complete is a BMS_Status bit.
 *
 * Issue 68 (ADR 0020): balancing through the step. The balancing logic
 * itself is test_bms_balance.c's.
 * - Only the paused (settled) Sweep feeds the charge taper and the
 *   end-of-charge test; every Sweep is still judged for the BMS faults.
 * - The open-wire check never runs in a step with a balancing command.
 * - The stop goes in the step BMS_OK goes low or the sense opens
 *   (EV.8.3.3); no FET is commanded on out of CHARGE.
 * - BMS_Balancing (0x600) every 1 s in Charge mode only, in the 0x202
 *   slot on step 26 of fifty; a STOPPED balancing is a BMS_Status bit and
 *   never a BMS fault.
 */
#include <string.h>

#include "bms_control.h"
#include "bms_tx.h"
#include "bq_sweep.h"
#include "can_control.h"
#include "cell_temp.h"
#include "e2e_p11.h"
#include "event_codes.h"
#include "mailbox.h"
#include "reset_cause.h"
#include "unity.h"

#define STEP_MS 20u

#define CELL_OK_MV 3700u
#define TEMP_OK_MV 1900u /* about 25 C */

static bms_control_t bc;
static bms_control_config_t config;
static bms_control_in_t in;
static bms_control_out_t out;
static bq_sweep_t sweep;
/* The charger's mailbox as control_bus_update() left it: NODATA, no
   charger, unless a test hears one. */
static mailbox_t charger;
static uint16_t chargerDa; /* its reported output current, 0.1 A */
static uint8_t chargerFlags;

/* The chain carries out every balancing command: each board's die at
   40 C, every FET checked good. */
static bq_balance_result_t balanceDone;

/* What the open-wire check finds whenever a step runs it. */
static bq_open_wire_t found;
/* BMS_OK as last written; the read-back follows it through the AND gate
   with NFAULT unless a test holds the read-back itself. */
static bool commanded;
static bool holdReadBack;
/* The Charging Shutdown Circuit closed but for BMS_OK, which is in its
   loop: the sense reads closed only while BMS_OK is high. */
static bool loopClosed;

static void good_sweep(void)
{
    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        sweep.cell_mv[i] = CELL_OK_MV;
        sweep.temp_mv[i] = TEMP_OK_MV;
    }
    sweep.devices_read = 0xFFFFu;
    sweep.bridge_read = true;
    sweep.bridge_fault = 0u;
}

void setUp(void)
{
    memset(&config, 0, sizeof config);
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    memset(&charger, 0, sizeof charger);
    chargerDa = 0u;
    chargerFlags = 0u;
    good_sweep();
    found = (bq_open_wire_t){ .verdict = BQ_OPEN_WIRE_PASS, .devices_read = 0xFFFFu };
    balanceDone = (bq_balance_result_t){
        .ok = true, .devices_read = 0xFFFFu, .fet = BQ_FET_CHECK_PASS,
        .die_dc = {400, 400, 400, 400, 400, 400, 400, 400,
                   400, 400, 400, 400, 400, 400, 400, 400},
    };
    commanded = false;
    holdReadBack = false;
    loopClosed = false;
    in.now_ms = 1000u;
    in.startup_ok = true;
    in.can_running = true;
    in.swept = true;
    in.sweep = &sweep;
    in.nfault_ok = true;
    in.charger = &charger;
    /* The unset parameters: 4.10 V, 2.0 A, 50 mV, 50-55 C, 10-5 C. */
    config.charge = (bms_charge_config_t){
        .target_mv = 4100u, .full_limit_da = 20u, .v_band_mv = 50u,
        .hot_full_dc = 500, .hot_zero_dc = 550, .cold_full_dc = 100, .cold_zero_dc = 50,
    };
    /* The unset parameters: the spec's tapers, 20.0 A, no compensation. */
    config.derate = (bms_derate_config_t){
        .v_full_mv = 3000u, .v_floor_mv = 2700u, .t_full_dc = 500, .t_floor_dc = 570,
        .cell_res_uohm = 0u, .full_limit_da = 200u,
    };
    bms_control_init(&bc, &config);
}

void tearDown(void) {}

/* One 20 ms step: the inputs read, the logic, BMS_OK written, then the
   open-wire check if it is due, its result for the next step. */
static void step(void)
{
    if (charger.state == MAILBOX_VALID) {
        /* A fresh frame from the charger each step it is heard. */
        const struct can_control_chg_status_t s = {
            .output_voltage = 4736u,
            .output_current = chargerDa,
            .over_temperature = (chargerFlags >> 1) & 1u,
        };
        TEST_ASSERT_EQUAL_INT(8, can_control_chg_status_pack(charger.data, &s, 8u));
    }
    if (loopClosed) {
        in.sdc_closed = commanded;
    }
    if (!holdReadBack) {
        in.bms_ok_fb = commanded && in.nfault_ok;
    }
    bms_control_step(&bc, &in, &out);
    commanded = out.bms_ok;
    in.open_wire_ran = out.open_wire_due;
    in.open_wire = found;
    /* Never both in one step: their worst cases together could outlast it. */
    TEST_ASSERT_FALSE(out.open_wire_due && out.balance_command.op != BQ_BALANCE_NONE);
    in.balance_ran = out.balance_command.op != BQ_BALANCE_NONE;
    in.balance_result = balanceDone;
    in.now_ms += STEP_MS;
    in.step_count++;
}

static void steps(unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        step();
    }
}

/* This step's frame with that ID, or NULL. */
static const bms_tx_frame_t *sent(uint32_t id)
{
    for (size_t i = 0; i < out.frame_count; i++) {
        if (out.frames[i].id == id) {
            return &out.frames[i];
        }
    }
    return NULL;
}

/* This step's frame with that ID, which must be there. */
static const uint8_t *frame(uint32_t id)
{
    const bms_tx_frame_t *f = sent(id);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "frame not sent on this step");
    TEST_ASSERT_FALSE(f->extended);
    TEST_ASSERT_EQUAL_UINT8(8u, f->len);
    return f->data;
}

static struct can_control_bms_status_t status(void)
{
    struct can_control_bms_status_t s;
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_status_unpack(&s, frame(0x080u), 8u));
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

/* The charger heard: its mailbox VALID with a good frame. */
static void hear_charger(void)
{
    charger.state = MAILBOX_VALID;
    charger.counters.good++;
}

/* Under-temperature (2 C, a reading at or above 2146 mV) is a fault in
   Charge mode only. */
static void test_under_temperature_is_raised_on_the_25th_sweep_in_charge_mode(void)
{
    hear_charger();
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

/* A cell under the Low-voltage limit (3.0 V) sets a status bit and never
   takes BMS_OK low. */
static void test_a_cell_under_the_low_voltage_limit_is_a_status_bit_and_no_fault(void)
{
    power_up();
    sweep.cell_mv[50] = 2999u;
    steps(200u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_TRUE(out.under_low_voltage_limit);
    TEST_ASSERT_EQUAL_UINT8(1u, status().low_voltage_limit);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);

    sweep.cell_mv[50] = 3000u;
    step();
    TEST_ASSERT_FALSE(out.under_low_voltage_limit);
}

/* The Low-voltage limit is the voltage taper's 100% point, a parameter:
   the bit says the Discharge limit is being derated (can/control.dbc). */
static void test_the_low_voltage_limit_follows_the_voltage_tapers_full_point(void)
{
    config.derate.v_full_mv = 3200u;
    bms_control_init(&bc, &config);
    power_up();
    sweep.cell_mv[50] = 3199u;
    step();
    TEST_ASSERT_TRUE(out.under_low_voltage_limit);
    TEST_ASSERT_EQUAL_UINT8(1u, status().low_voltage_limit);
    TEST_ASSERT_LESS_THAN_UINT16(200u, out.discharge_limit_da);
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

/* The Held fault (ADR 0007, 0011): after any reset that was not a
   Power-on reset (BOR flag set). The reset-cause register's flags as
   RM0440 7.4.28 gives them, classified by reset_cause.h. */
#define CSR_OBLRSTF  (1u << 25)
#define CSR_PINRSTF  (1u << 26)
#define CSR_BORRSTF  (1u << 27)
#define CSR_SFTRSTF  (1u << 28)
#define CSR_IWDGRSTF (1u << 29)
#define CSR_WWDGRSTF (1u << 30)
#define CSR_LPWRRSTF (1u << 31)

static bool held_after(uint32_t csr, bool debugger)
{
    return bms_control_held(reset_cause_classify(csr), debugger);
}

static void test_every_reset_but_power_on_is_held(void)
{
    TEST_ASSERT_TRUE(held_after(CSR_IWDGRSTF | CSR_PINRSTF, false)); /* the hang */
    TEST_ASSERT_TRUE(held_after(CSR_SFTRSTF | CSR_PINRSTF, false));  /* the crash routine */
    TEST_ASSERT_TRUE(held_after(CSR_PINRSTF, false));                /* the reset pin */
    TEST_ASSERT_TRUE(held_after(CSR_OBLRSTF | CSR_PINRSTF, false));  /* option-byte load */
    TEST_ASSERT_TRUE(held_after(CSR_WWDGRSTF | CSR_PINRSTF, false));
    TEST_ASSERT_TRUE(held_after(CSR_LPWRRSTF | CSR_PINRSTF, false));
    TEST_ASSERT_TRUE(held_after(0u, false)); /* no flag: not shown to be power-on */
}

/* BOR set is a Power-on reset (a supply sag included): a normal start,
   whatever else is set with it. */
static void test_a_power_on_reset_is_not_held_whatever_else_is_set(void)
{
    TEST_ASSERT_FALSE(held_after(CSR_BORRSTF | CSR_PINRSTF, false));
    TEST_ASSERT_FALSE(held_after(CSR_BORRSTF | CSR_IWDGRSTF | CSR_SFTRSTF | CSR_PINRSTF, false));
}

/* With the core debug enable bit set, any reset is a Power-on reset. */
static void test_no_reset_is_held_under_the_debugger(void)
{
    TEST_ASSERT_FALSE(held_after(CSR_IWDGRSTF | CSR_PINRSTF, true));
    TEST_ASSERT_FALSE(held_after(CSR_SFTRSTF | CSR_PINRSTF, true));
    TEST_ASSERT_FALSE(held_after(CSR_PINRSTF, true));
    TEST_ASSERT_FALSE(held_after(CSR_OBLRSTF | CSR_PINRSTF, true));
    TEST_ASSERT_FALSE(held_after(0u, true));
    TEST_ASSERT_FALSE(held_after(CSR_BORRSTF | CSR_PINRSTF, true));
}

/* The Held fault: FAULT_HELD from the first step with the internal fault,
   its Event naming the Held fault, and no recovery. */
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
    TEST_ASSERT_EQUAL_UINT8(EVENT_BMS_HELD, out.events[0].data[2]);

    steps(500u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT_HELD, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_TRUE(out.fault_active);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_INTERNAL, out.active);
    TEST_ASSERT_EQUAL_UINT8(1u, status().internal_active);
    TEST_ASSERT_EQUAL_size_t(0u, out.event_count);
}

/* BMS_OK never rises after a held reset, at any step, though everything
   a normal start needs is there: only a Power-on reset leaves FAULT_HELD. */
static void test_bms_ok_never_rises_while_held(void)
{
    config.held = true;
    bms_control_init(&bc, &config);
    in.sdc_closed = true;
    for (unsigned i = 0; i < 3000u; i++) { /* a minute */
        step();
        TEST_ASSERT_FALSE(out.bms_ok);
        TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT_HELD, out.state);
        TEST_ASSERT_TRUE(out.fault_active);
    }
    TEST_ASSERT_EQUAL_UINT8(1u, status().internal_active);
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
        TEST_ASSERT_EQUAL(E2E_P11_OK, e2e_p11_check(&rx, 0x0080u, frame(0x080u), 8u));
    }
    TEST_ASSERT_EQUAL_UINT8(1u, status().parameters_defaulted);
    TEST_ASSERT_EQUAL_UINT8(1u, status().last_reset_not_power_on);
    TEST_ASSERT_EQUAL_UINT8(0u, status().not_dual_bank);
    TEST_ASSERT_EQUAL_UINT8(1u, status().shutdown_circuit_closed);
}

/* ---- issue 64: the second layers ----------------------------------------- */

/* This step's Event for fault, which must be there. */
static const uint8_t *event_for(bms_fault_t fault)
{
    for (size_t i = 0; i < out.event_count; i++) {
        if (out.events[i].code == EVENT_BMS_FAULT && out.events[i].data[0] == fault) {
            return out.events[i].data;
        }
    }
    TEST_FAIL_MESSAGE("no Event for the fault");
    return NULL;
}

static void assert_where(const uint8_t *d, uint8_t where, uint16_t index, uint16_t reading)
{
    TEST_ASSERT_EQUAL_UINT8(where, d[2]);
    TEST_ASSERT_EQUAL_UINT16(index, (uint16_t)(d[4] | (d[5] << 8)));
    TEST_ASSERT_EQUAL_UINT16(reading, (uint16_t)(d[6] | (d[7] << 8)));
}

static bool active(bms_fault_t fault)
{
    return (out.active & (1u << fault)) != 0u;
}

/* Steps until one runs the open-wire check, then once more: the step that
   takes its result. */
static void until_a_check_is_judged(void)
{
    for (int i = 0; i < 100 && !out.open_wire_due; i++) {
        step();
    }
    TEST_ASSERT_TRUE(out.open_wire_due);
    step();
}

static void test_the_open_wire_check_runs_on_the_first_sweep_then_once_a_second(void)
{
    in.swept = false; /* the chain is waking */
    steps(3u);
    TEST_ASSERT_FALSE(out.open_wire_due);

    in.swept = true;
    const uint32_t first = in.now_ms;
    step();
    TEST_ASSERT_TRUE(out.open_wire_due);

    while (in.now_ms - first < 1000u) {
        step();
        TEST_ASSERT_FALSE(out.open_wire_due);
    }
    step();
    TEST_ASSERT_TRUE(out.open_wire_due);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* "never in place of a sweep": a step that took none runs no check, and
   the check due waits for the next that did. */
static void test_the_open_wire_check_runs_only_after_a_sweep(void)
{
    power_up();
    until_a_check_is_judged();
    steps(48u);
    in.swept = false;
    steps(3u);
    TEST_ASSERT_FALSE(out.open_wire_due);
    in.swept = true;
    step();
    TEST_ASSERT_TRUE(out.open_wire_due);
}

/* Power-up (spec #36): BMS_OK rises only after one open-wire check has
   passed, however clean the Sweeps. */
static void test_bms_ok_never_rises_before_an_open_wire_check_has_passed(void)
{
    found.verdict = BQ_OPEN_WIRE_UNKNOWN;
    steps(100u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);

    found.verdict = BQ_OPEN_WIRE_PASS;
    until_a_check_is_judged();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
}

/* EV.8.3.4 b: a blown or tripped voltage sense fuse reads as an open sense
   wire. Two failed checks in a row, a second apart, are the fault. */
static void test_two_failed_open_wire_checks_in_a_row_are_the_open_sense_wire_fault(void)
{
    power_up();
    until_a_check_is_judged();
    found = (bq_open_wire_t){
        .verdict = BQ_OPEN_WIRE_FAIL, .device = 6u, .cell = 42u, .devices_read = 0xFFFFu,
    };

    until_a_check_is_judged();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);

    until_a_check_is_judged();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_OPEN_WIRE, out.active);
    assert_where(event_for(BMS_FAULT_OPEN_WIRE), EVENT_BMS_CELL, 42u, 0u);
    TEST_ASSERT_EQUAL_UINT8(1u, status().open_wire_active);
    TEST_ASSERT_EQUAL_UINT8(1u, status().open_wire_seen);
}

/* A device that did not finish the comparison is named by its number. */
static void test_a_device_that_did_not_finish_is_named_in_the_event(void)
{
    power_up();
    found = (bq_open_wire_t){ .verdict = BQ_OPEN_WIRE_FAIL, .device = 12u, .devices_read = 0xFFFFu };
    until_a_check_is_judged();
    until_a_check_is_judged();
    TEST_ASSERT_TRUE(active(BMS_FAULT_OPEN_WIRE));
    assert_where(event_for(BMS_FAULT_OPEN_WIRE), EVENT_BMS_DEVICE, 12u, 0u);
}

static void test_one_failed_check_then_a_pass_is_no_fault(void)
{
    power_up();
    found.verdict = BQ_OPEN_WIRE_FAIL;
    until_a_check_is_judged();
    found.verdict = BQ_OPEN_WIRE_PASS;
    until_a_check_is_judged();
    found.verdict = BQ_OPEN_WIRE_FAIL;
    until_a_check_is_judged();
    steps(40u); /* to just before the next check */
    TEST_ASSERT_FALSE(out.open_wire_due);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);
}

/* A check that could not judge (a device silent: the measurement fault's
   business) neither counts as a failure nor ends a run of them. */
static void test_a_check_that_could_not_judge_neither_counts_nor_clears(void)
{
    power_up();
    found.verdict = BQ_OPEN_WIRE_FAIL;
    until_a_check_is_judged();
    found.verdict = BQ_OPEN_WIRE_UNKNOWN;
    until_a_check_is_judged();
    TEST_ASSERT_FALSE(active(BMS_FAULT_OPEN_WIRE));
    found.verdict = BQ_OPEN_WIRE_FAIL;
    until_a_check_is_judged();
    TEST_ASSERT_TRUE(active(BMS_FAULT_OPEN_WIRE));
}

/* The budget (spec #36): BMS_OK low within 2.5 s of a wire opening, at
   worst just after a check passed. */
static void test_an_open_wire_takes_bms_ok_low_within_2_5_s(void)
{
    power_up();
    until_a_check_is_judged();
    const uint32_t opened = in.now_ms - 2u * STEP_MS; /* as the passing check ran */
    found.verdict = BQ_OPEN_WIRE_FAIL;

    while (out.bms_ok && in.now_ms - opened < 5000u) {
        step();
    }
    TEST_ASSERT_FALSE(out.bms_ok);
    /* in.now_ms is already the next step's time. */
    TEST_ASSERT_TRUE(in.now_ms - STEP_MS - opened <= 2500u);
}

/* Firmware does not latch: a second of passing ends the fault. */
static void test_the_open_wire_fault_ends_once_checks_pass_again(void)
{
    power_up();
    found.verdict = BQ_OPEN_WIRE_FAIL;
    until_a_check_is_judged();
    until_a_check_is_judged();
    TEST_ASSERT_TRUE(active(BMS_FAULT_OPEN_WIRE));

    found.verdict = BQ_OPEN_WIRE_PASS;
    until_a_check_is_judged(); /* the first of 50 clean Sweeps */
    steps(BMS_RECOVER_SWEEPS - 2u);
    TEST_ASSERT_TRUE(active(BMS_FAULT_OPEN_WIRE));
    step();
    TEST_ASSERT_FALSE(active(BMS_FAULT_OPEN_WIRE));
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* The cell monitor fault: the comparators, through NFAULT. */
static void test_nfault_low_is_the_cell_monitor_fault_at_once(void)
{
    power_up();
    in.nfault_ok = false;
    sweep.bridge_fault = 0x08u; /* the register agrees */
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_CELL_MONITOR, out.active);
    assert_where(event_for(BMS_FAULT_CELL_MONITOR), EVENT_BMS_NFAULT, 0u, 0x08u);
    TEST_ASSERT_EQUAL_UINT8(1u, status().cell_monitor_active);

    /* NFAULT takes BMS_OK low through the AND gate: no internal fault. */
    steps(100u);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_CELL_MONITOR, out.seen);
}

static void test_a_device_reporting_its_own_fault_is_the_cell_monitor_fault(void)
{
    power_up();
    sweep.bridge_fault = 0x02u; /* FAULT_SYS */
    step();
    TEST_ASSERT_TRUE(active(BMS_FAULT_CELL_MONITOR));
    assert_where(event_for(BMS_FAULT_CELL_MONITOR), EVENT_BMS_BRIDGE, 0u, 0x02u);
}

/* Spec #36: post-wake faults are cleared before BMS_OK may rise; one the
   clear did not end keeps it low. */
static void test_bms_ok_does_not_rise_over_a_fault_left_from_the_wake(void)
{
    sweep.bridge_fault = 0x01u;
    in.nfault_ok = false;
    steps(200u);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_TRUE(active(BMS_FAULT_CELL_MONITOR));
}

/* Before the chain is awake NFAULT means nothing: no Sweep, no fault. */
static void test_nfault_low_while_the_chain_wakes_is_no_fault(void)
{
    in.swept = false;
    in.nfault_ok = false;
    steps(50u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);
}

/* A stuck MCU_BMS_OK, or a broken AND gate: 5 Sweeps of the read-back
   differing from commanded AND NFAULT. */
static void test_a_bms_ok_read_back_wrong_for_5_sweeps_is_the_internal_fault(void)
{
    power_up();
    holdReadBack = true;
    in.bms_ok_fb = false; /* commanded high, NFAULT high */
    steps(BMS_FEEDBACK_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);

    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_INTERNAL, out.active);
    /* read-back 0, commanded 1 (bit 1), NFAULT 1 (bit 2) */
    assert_where(event_for(BMS_FAULT_INTERNAL), EVENT_BMS_FEEDBACK, 0u, 0x06u);
    TEST_ASSERT_EQUAL_UINT8(1u, status().internal_active);
}

static void test_a_read_back_high_while_commanded_low_is_the_internal_fault(void)
{
    found.verdict = BQ_OPEN_WIRE_UNKNOWN; /* STARTUP: BMS_OK commanded low */
    holdReadBack = true;
    in.bms_ok_fb = true;
    steps(BMS_FEEDBACK_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);
    step();
    TEST_ASSERT_TRUE(active(BMS_FAULT_INTERNAL));
}

/* "a mismatch for 5 sweeps": a step that took no Sweep neither counts one
   nor ends the run. */
static void test_the_read_back_check_counts_sweeps_not_steps(void)
{
    power_up();
    holdReadBack = true;
    in.bms_ok_fb = false;
    steps(BMS_FEEDBACK_SWEEPS - 1u);
    in.swept = false;
    steps(3u);
    TEST_ASSERT_FALSE(active(BMS_FAULT_INTERNAL));
    in.swept = true;
    step();
    TEST_ASSERT_TRUE(active(BMS_FAULT_INTERNAL));
}

/* Firmware does not latch (spec #36): once the chips let NFAULT go (each
   open-wire check clears what they latched, bq_chain.h), a second clean
   ends the cell monitor fault. */
static void test_a_comparator_trip_ends_a_second_after_nfault_rises(void)
{
    power_up();
    in.nfault_ok = false;
    sweep.bridge_fault = 0x04u;
    steps(30u);
    TEST_ASSERT_TRUE(active(BMS_FAULT_CELL_MONITOR));

    in.nfault_ok = true;
    sweep.bridge_fault = 0u;
    steps(BMS_RECOVER_SWEEPS - 1u);
    TEST_ASSERT_TRUE(active(BMS_FAULT_CELL_MONITOR));
    step();
    TEST_ASSERT_FALSE(active(BMS_FAULT_CELL_MONITOR));
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* A dead bridge reads "no fault" on NFAULT, through its pull-up (ADR
   0008): its register over SPI must agree. */
static void test_nfault_disagreeing_with_the_bridge_register_for_5_sweeps_is_internal(void)
{
    power_up();
    sweep.bridge_fault = 0x04u; /* FAULT_REG; NFAULT still reads high */
    steps(BMS_FEEDBACK_SWEEPS - 1u);
    TEST_ASSERT_TRUE(active(BMS_FAULT_CELL_MONITOR));
    TEST_ASSERT_FALSE(active(BMS_FAULT_INTERNAL));

    step();
    TEST_ASSERT_TRUE(active(BMS_FAULT_INTERNAL));
    assert_where(event_for(BMS_FAULT_INTERNAL), EVENT_BMS_NFAULT_CHECK, 0u, 0x04u);
}

static void test_nfault_low_with_a_clear_register_is_internal_too(void)
{
    power_up();
    in.nfault_ok = false;
    steps(BMS_FEEDBACK_SWEEPS);
    TEST_ASSERT_TRUE(active(BMS_FAULT_INTERNAL));
}

/* The register is only read with a Sweep: a step without one neither
   counts a disagreement nor clears the count. */
static void test_the_nfault_cross_check_waits_for_the_register(void)
{
    power_up();
    sweep.bridge_fault = 0x04u;
    steps(BMS_FEEDBACK_SWEEPS - 1u);
    in.swept = false;
    steps(2u);
    TEST_ASSERT_FALSE(active(BMS_FAULT_INTERNAL));
    in.swept = true;
    step();
    TEST_ASSERT_TRUE(active(BMS_FAULT_INTERNAL));
}

/* BMS_OK low and the Shutdown Circuit still energised 500 ms later: the
   BMS's output path does not reach the shutdown loop. */
static void test_bms_ok_low_for_500_ms_with_the_sense_energised_is_internal(void)
{
    in.swept = false; /* STARTUP: BMS_OK low from the first step */
    in.sdc_closed = true;
    steps(500u / STEP_MS);
    TEST_ASSERT_FALSE(active(BMS_FAULT_INTERNAL));

    step(); /* 500 ms after the first */
    TEST_ASSERT_TRUE(active(BMS_FAULT_INTERNAL));
    assert_where(event_for(BMS_FAULT_INTERNAL), EVENT_BMS_SDC_SENSE, 0u, 500u);
}

static void test_a_shutdown_circuit_that_opens_inside_500_ms_is_no_fault(void)
{
    power_up();
    in.nfault_ok = false; /* the comparators take BMS_OK low */
    sweep.bridge_fault = 0x08u;
    in.sdc_closed = true;
    steps(480u / STEP_MS);
    in.sdc_closed = false; /* the loop opened: the path works */
    steps(200u);
    TEST_ASSERT_FALSE(active(BMS_FAULT_INTERNAL));
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen & (1u << BMS_FAULT_INTERNAL));
}

/* Issue 65: three Overruns within one second (the runner's timing fault,
   true for one step) are the BMS internal fault, which recovers after 1 s
   clean like any other. The Overrun Events carry the specific cause. */
static void test_a_timing_fault_is_the_internal_fault_at_once(void)
{
    power_up();
    in.timing_fault = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_TRUE(out.fault_active);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_INTERNAL, out.active);
    TEST_ASSERT_EQUAL_size_t(1u, out.event_count);
    assert_where(event_for(BMS_FAULT_INTERNAL), EVENT_BMS_TIMING, 0u, 0u);
    TEST_ASSERT_EQUAL_UINT8(1u, status().internal_active);
}

static void test_a_timing_fault_recovers_after_exactly_1_s_clean(void)
{
    power_up();
    in.timing_fault = true;
    step();
    in.timing_fault = false;
    steps(BMS_RECOVER_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_TRUE(active(BMS_FAULT_INTERNAL));
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_FALSE(active(BMS_FAULT_INTERNAL));
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_INTERNAL, out.seen);
}

/* Each further Overrun with two others in the second before it is the
   timing fault again: the clean second starts again, with no new Event. */
static void test_a_further_timing_fault_starts_the_clean_second_again(void)
{
    power_up();
    in.timing_fault = true;
    step();
    in.timing_fault = false;
    steps(BMS_RECOVER_SWEEPS - 1u);
    in.timing_fault = true;
    step();
    TEST_ASSERT_EQUAL_size_t(0u, out.event_count);
    in.timing_fault = false;
    steps(BMS_RECOVER_SWEEPS - 1u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_DRIVE, out.state);
}

/* ---- issue 66: the Discharge limit and the send table ------------------ */

/* The unset parameters but for these two. */
static void derating(uint16_t cell_res_uohm, uint16_t full_limit_da)
{
    config.derate.cell_res_uohm = cell_res_uohm;
    config.derate.full_limit_da = full_limit_da;
    bms_control_init(&bc, &config);
}

static struct can_control_bms_power_t power(void)
{
    struct can_control_bms_power_t p;
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_power_unpack(&p, frame(0x082u), 8u));
    return p;
}

/* Steps until the slower frame with that ID goes, which must be within
   five steps. */
static const uint8_t *step_until(uint32_t id)
{
    for (unsigned i = 0; i < 5u; i++) {
        step();
        if (sent(id) != NULL) {
            return frame(id);
        }
    }
    TEST_FAIL_MESSAGE("not sent within five steps");
    return NULL;
}

static void test_every_five_steps_send_status_and_power_each_step_and_each_slow_frame_once(void)
{
    static const uint32_t slow[] = { 0x081u, 0x202u, 0x60Du, 0x210u, 0x211u };
    for (unsigned cycle = 0; cycle < 3u; cycle++) {
        unsigned seen[5] = {0};
        for (unsigned i = 0; i < 5u; i++) {
            step();
            TEST_ASSERT_EQUAL_size_t(3u, out.frame_count);
            TEST_ASSERT_NOT_NULL(sent(0x080u));
            TEST_ASSERT_NOT_NULL(sent(0x082u));
            for (unsigned f = 0; f < 5u; f++) {
                seen[f] += (sent(slow[f]) != NULL) ? 1u : 0u;
            }
        }
        for (unsigned f = 0; f < 5u; f++) {
            TEST_ASSERT_EQUAL_UINT(1u, seen[f]);
        }
    }
}

static void test_the_discharge_limit_is_zero_until_bms_ok_is_up(void)
{
    derating(0u, 200u);
    for (unsigned i = 0; i < BMS_POWERUP_SWEEPS - 1u; i++) {
        step();
        TEST_ASSERT_FALSE(out.bms_ok);
        TEST_ASSERT_EQUAL_UINT16(0u, power().discharge_limit);
    }
    step();
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_UINT16(200u, power().discharge_limit); /* 20.0 A */
}

/* BMS_Power carries the Discharge limit, pack current and pack voltage of
   one Sweep, protected by E2E Profile 11 (Data ID 0x0082). */
static void test_bms_power_carries_the_limit_the_current_and_the_pack_voltage(void)
{
    derating(0u, 200u);
    in.pack_current = (bms_current_t){ .deci_a = 1234, .valid = true };
    e2e_p11_rx_t rx = {0};
    for (unsigned i = 0; i < 10u; i++) {
        step();
        TEST_ASSERT_EQUAL(E2E_P11_OK, e2e_p11_check(&rx, 0x0082u, frame(0x082u), 8u));
    }
    TEST_ASSERT_EQUAL_UINT16(200u, power().discharge_limit);
    TEST_ASSERT_EQUAL_INT16(1234, power().pack_current);
    TEST_ASSERT_EQUAL_UINT16(4736u, power().pack_voltage); /* 128 x 3.700 V */
}

/* Issue 29 decides how an invalid current is marked; until then it goes
   as 0 A. */
static void test_an_invalid_pack_current_goes_as_zero(void)
{
    in.pack_current = (bms_current_t){ .deci_a = 1234, .valid = false };
    step();
    TEST_ASSERT_EQUAL_INT16(0, power().pack_current);
}

/* A weak cell lowers the limit, by no more than 4% a step. */
static void test_a_weak_cell_lowers_the_limit_in_bms_power(void)
{
    derating(0u, 200u);
    power_up();
    sweep.cell_mv[50] = 2850u; /* 60% */
    step();
    TEST_ASSERT_EQUAL_UINT16(192u, power().discharge_limit); /* 96% of 20 A */
    steps(20u);
    TEST_ASSERT_EQUAL_UINT16(120u, power().discharge_limit); /* 60% */
}

/* 0x202 carries the same limit in whole amps (ADR 0017: unscaled), and
   0 A to charge in DRIVE. */
static void test_the_inverter_gets_the_limit_in_whole_amps_and_no_charge(void)
{
    derating(0u, 200u);
    power_up();
    sweep.cell_mv[50] = 2850u;
    steps(30u);
    struct can_control_inv_bms_limits_t inv;
    TEST_ASSERT_EQUAL_INT(0, can_control_inv_bms_limits_unpack(&inv, step_until(0x202u), 8u));
    TEST_ASSERT_EQUAL_UINT16(12u, inv.discharge_limit);
    TEST_ASSERT_EQUAL_UINT16(0u, inv.charge_limit);
}

/* Rounded up, so the inverter's backup sits at or just above the limit the
   VCU obeys and never cuts under it (ADR 0017). */
static void test_the_inverter_limit_rounds_up_to_the_whole_amp(void)
{
    derating(0u, 205u); /* 20.5 A */
    power_up();
    struct can_control_inv_bms_limits_t inv;
    TEST_ASSERT_EQUAL_INT(0, can_control_inv_bms_limits_unpack(&inv, step_until(0x202u), 8u));
    TEST_ASSERT_EQUAL_UINT16(21u, inv.discharge_limit);
    TEST_ASSERT_EQUAL_UINT16(205u, power().discharge_limit);
}

static void test_the_inverter_limit_is_zero_until_bms_ok_is_up(void)
{
    derating(0u, 200u);
    struct can_control_inv_bms_limits_t inv;
    TEST_ASSERT_EQUAL_INT(0, can_control_inv_bms_limits_unpack(&inv, step_until(0x202u), 8u));
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_EQUAL_UINT16(0u, inv.discharge_limit);
}

/* BMS_Limits: the charge limit, 0 A in DRIVE, and the pack voltage,
   protected by E2E Profile 11 (Data ID 0x0081). */
static void test_bms_limits_says_no_charge_in_drive(void)
{
    derating(0u, 200u);
    power_up();
    e2e_p11_rx_t rx = {0};
    struct can_control_bms_limits_t lim;
    const uint8_t *f = step_until(0x081u);
    TEST_ASSERT_EQUAL(E2E_P11_OK, e2e_p11_check(&rx, 0x0081u, f, 8u));
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_limits_unpack(&lim, f, 8u));
    TEST_ASSERT_EQUAL_UINT16(0u, lim.charge_limit);
    TEST_ASSERT_EQUAL_UINT16(4736u, lim.pack_voltage);
}

/* ---- issue 67: charge control ------------------------------------------ */

#define CHG_REQUEST_ID   0x1806E5F4u
#define CHG_PERIOD_STEPS 50u

static struct can_control_chg_request_t chg_request(void)
{
    const bms_tx_frame_t *f = sent(CHG_REQUEST_ID);
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "no charger request on this step");
    TEST_ASSERT_TRUE(f->extended);
    TEST_ASSERT_EQUAL_UINT8(8u, f->len);
    struct can_control_chg_request_t r;
    TEST_ASSERT_EQUAL_INT(0, can_control_chg_request_unpack(&r, f->data, 8u));
    return r;
}

/* Steps until the charger request goes, which must be within a second. */
static struct can_control_chg_request_t step_until_request(void)
{
    for (unsigned i = 0; i < CHG_PERIOD_STEPS; i++) {
        step();
        if (sent(CHG_REQUEST_ID) != NULL) {
            return chg_request();
        }
    }
    TEST_FAIL_MESSAGE("no charger request within a second");
    return chg_request();
}

/* On the hand cart: the charger heard and the Charging Shutdown Circuit
   closed. */
static void power_up_charge(void)
{
    hear_charger();
    loopClosed = true;
    steps(BMS_POWERUP_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
}

/* Past the 3 s start, to the first request that asks for current. */
static struct can_control_chg_request_t start_charge(void)
{
    power_up_charge();
    steps(BMS_CHARGE_START_MS / STEP_MS);
    const struct can_control_chg_request_t r = step_until_request();
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_START_CHOICE, r.control);
    return r;
}

static bool any_event(uint16_t code)
{
    for (size_t i = 0; i < out.event_count; i++) {
        if (out.events[i].code == code) {
            return true;
        }
    }
    return false;
}

static void test_hearing_the_charger_is_charge_mode_until_a_power_cycle(void)
{
    power_up_charge();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, status().state);
    TEST_ASSERT_TRUE(out.bms_ok);

    /* The charger timeout: still CHARGE, BMS_OK high, no fault. */
    charger.state = MAILBOX_INVALID;
    steps(1000u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);
}

/* Once a second, on step 1 of the second, in place of 0x202; the other
   nine 0x202 slots keep it; never more than three frames a step. */
static void test_in_charge_the_request_goes_once_a_second_in_the_0x202_slot(void)
{
    power_up_charge();
    unsigned requests = 0u;
    unsigned inverter = 0u;
    for (unsigned i = 0; i < 3u * CHG_PERIOD_STEPS; i++) {
        const uint32_t n = in.step_count;
        step();
        TEST_ASSERT_LESS_OR_EQUAL_size_t(3u, out.frame_count);
        if (sent(CHG_REQUEST_ID) != NULL) {
            requests++;
            TEST_ASSERT_EQUAL_UINT32(1u, n % CHG_PERIOD_STEPS);
            TEST_ASSERT_NULL(sent(0x202u));
        }
        inverter += (sent(0x202u) != NULL) ? 1u : 0u;
    }
    TEST_ASSERT_EQUAL_UINT(3u, requests);
    TEST_ASSERT_EQUAL_UINT(24u, inverter); /* and three BMS_Balancing */
}

/* ---- balancing (issue 68) ----------------------------------------------- */

#define BALANCING_ID 0x600u

/* Near the Charge target with a 20 mV spread: past the Balancing gate. */
static void spread_cells(void)
{
    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        sweep.cell_mv[i] = 4070u;
    }
    sweep.cell_mv[0] = 4050u;
}

static bool bleeding(void)
{
    for (size_t d = 0; d < BQ_SWEEP_DEVICES; d++) {
        if (out.bleeding[d] != 0u) {
            return true;
        }
    }
    return false;
}

/* Charging, then on until a cell bleeds; fails rather than hangs. */
static void charge_until_bleeding(void)
{
    spread_cells();
    (void)start_charge();
    for (unsigned i = 0; i < 1000u && !bleeding(); i++) {
        step();
    }
    TEST_ASSERT_TRUE(bleeding());
}

static void test_balancing_status_goes_once_a_second_in_charge_mode_only(void)
{
    power_up();
    for (unsigned i = 0; i < 500u; i++) {
        step();
        TEST_ASSERT_NULL(sent(BALANCING_ID));
    }

    setUp();
    spread_cells();
    power_up_charge();
    unsigned n = 0u;
    for (unsigned i = 0; i < 3u * CHG_PERIOD_STEPS; i++) {
        const uint32_t count = in.step_count;
        step();
        TEST_ASSERT_LESS_OR_EQUAL_size_t(3u, out.frame_count);
        if (sent(BALANCING_ID) != NULL) {
            n++;
            TEST_ASSERT_EQUAL_UINT32(26u, count % CHG_PERIOD_STEPS);
            struct can_control_bms_balancing_t f;
            TEST_ASSERT_EQUAL_INT(0, can_control_bms_balancing_unpack(&f, frame(BALANCING_ID), 8u));
            TEST_ASSERT_EQUAL_UINT8(out.balance_state, f.state);
            TEST_ASSERT_EQUAL_UINT16(20u, f.cell_spread);
        }
    }
    TEST_ASSERT_EQUAL_UINT(3u, n);
}

/* EV.8.3.3: the stop goes in the step the sense opens or BMS_OK goes low,
   and while out of CHARGE no FET is commanded on. */
static void test_balancing_stops_in_the_step_the_sense_opens(void)
{
    charge_until_bleeding();
    loopClosed = false;
    in.sdc_closed = false;
    step();
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.balance_command.op);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_PAUSED, out.balance_state);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_SENSE_OPEN, out.balance_reason);
}

static void test_balancing_stops_in_the_step_a_bms_fault_takes_bms_ok_low(void)
{
    charge_until_bleeding();
    sweep.devices_read = 0x7FFFu; /* a device missing: the measurement fault */
    for (unsigned i = 0; i < BMS_MEAS_SWEEPS && out.state == BMS_STATE_CHARGE; i++) {
        step();
    }
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.balance_command.op);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_BMS_FAULT, out.balance_reason);
    step();
    TEST_ASSERT_FALSE(bleeding());
    for (unsigned i = 0; i < 200u; i++) {
        step();
        TEST_ASSERT_NOT_EQUAL(BQ_BALANCE_SET, out.balance_command.op);
        TEST_ASSERT_NOT_EQUAL(BQ_BALANCE_FET_ON, out.balance_command.op);
    }
}

/* Only the paused Sweep feeds the charge: a highest cell that reads at
   the target only while cells bleed does not move the Charge limit. */
static void test_the_charge_sees_only_the_settled_sweep(void)
{
    charge_until_bleeding();
    step();
    const uint16_t limit = out.charge_limit_da;
    sweep.cell_mv[100] = 4100u;
    unsigned unsettled = 0u;
    while (bleeding()) {
        step();
        TEST_ASSERT_EQUAL_UINT16(limit, out.charge_limit_da);
        unsettled++;
    }
    TEST_ASSERT_TRUE(unsettled > 40u);
    for (unsigned i = 0; i < 5u; i++) {
        step(); /* the pause, then the settled Sweep */
    }
    TEST_ASSERT_TRUE(out.charge_limit_da < limit);
}

static void test_a_stopped_balancing_is_a_status_bit_and_no_bms_fault(void)
{
    spread_cells();
    sweep.cell_mv[0] = 3890u; /* more than 200 mV under the 4.10 V target */
    power_up_charge();
    for (unsigned i = 0; i < 500u && out.balance_state != BMS_BALANCE_STOPPED; i++) {
        step();
    }
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_STOPPED, out.balance_state);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_LOW_CELL, out.balance_reason);
    TEST_ASSERT_EQUAL_UINT8(1u, status().balancing_stopped);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);
}

/* "No request is ever sent outside CHARGE": in DRIVE, in STARTUP with the
   charger heard, and in FAULT in Charge mode. */
static void test_no_charger_request_is_ever_sent_outside_charge(void)
{
    power_up();
    for (unsigned i = 0; i < 500u; i++) {
        step();
        TEST_ASSERT_NULL(sent(CHG_REQUEST_ID));
    }

    setUp();
    hear_charger();
    loopClosed = true;
    for (unsigned i = 0; i < BMS_POWERUP_SWEEPS - 1u; i++) {
        step(); /* step 1, the request's slot, is in STARTUP */
        TEST_ASSERT_EQUAL_UINT8(BMS_STATE_STARTUP, out.state);
        TEST_ASSERT_NULL(sent(CHG_REQUEST_ID));
    }

    setUp();
    start_charge();
    sweep.cell_mv[3] = BMS_OV_MV;
    steps(BMS_OV_SWEEPS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    for (unsigned i = 0; i < 500u; i++) {
        step();
        TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
        TEST_ASSERT_NULL(sent(CHG_REQUEST_ID));
    }
}

/* Held (FAULT_HELD) with the charger heard: never a request. */
static void test_no_charger_request_is_sent_in_fault_held(void)
{
    config.held = true;
    bms_control_init(&bc, &config);
    hear_charger();
    loopClosed = true;
    for (unsigned i = 0; i < 500u; i++) {
        step();
        TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT_HELD, out.state);
        TEST_ASSERT_NULL(sent(CHG_REQUEST_ID));
    }
}

/* The charger timeout mid-charge: CHARGE and BMS_OK kept, a stop every
   second; the charger back, the charge starts again after 3 s. */
static void test_a_silent_charger_gets_stop_every_second_and_its_return_restarts_the_charge(void)
{
    start_charge();
    charger.state = MAILBOX_INVALID;
    for (unsigned s = 0; s < 10u; s++) {
        const struct can_control_chg_request_t r = step_until_request();
        TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE, r.control);
        TEST_ASSERT_EQUAL_UINT16(0u, r.max_charge_current);
        TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
        TEST_ASSERT_TRUE(out.bms_ok);
    }
    hear_charger();
    steps(BMS_CHARGE_START_MS / STEP_MS);
    const struct can_control_chg_request_t r = step_until_request();
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_START_CHOICE, r.control);
    TEST_ASSERT_EQUAL_UINT16(20u, r.max_charge_current);
}

/* Waiting it asks for nothing; charging, for the Charge limit at 128 x
   the Charge target; BMS_Limits carries the Charge limit. */
static void test_a_charge_through_the_step_asks_for_the_charge_limit(void)
{
    power_up_charge();
    struct can_control_chg_request_t r = step_until_request();
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE, r.control);
    TEST_ASSERT_EQUAL_UINT16(0u, r.max_charge_voltage);
    TEST_ASSERT_EQUAL_UINT16(0u, r.max_charge_current);

    setUp();
    r = start_charge();
    TEST_ASSERT_EQUAL_UINT16(5248u, r.max_charge_voltage); /* 524.8 V */
    TEST_ASSERT_EQUAL_UINT16(20u, r.max_charge_current);   /* 2.0 A */

    struct can_control_bms_limits_t lim;
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_limits_unpack(&lim, step_until(0x081u), 8u));
    TEST_ASSERT_EQUAL_UINT16(20u, lim.charge_limit);
}

/* 1.1 A over the 2.0 A asked for, for 5 s: a BMS fault, BMS_OK low. */
static void test_charger_disobeys_is_a_bms_fault(void)
{
    start_charge();
    chargerDa = 31u;
    steps(BMS_CHARGE_DISOBEY_MS / STEP_MS);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_FAULT, out.state);
    TEST_ASSERT_FALSE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(1u << BMS_FAULT_CHARGER_DISOBEYS, out.active);
    TEST_ASSERT_EQUAL_UINT8(1u, status().charger_disobeys_active);

    TEST_ASSERT_EQUAL_UINT16(EVENT_BMS_FAULT, out.events[0].code);
    const uint8_t *d = out.events[0].data;
    TEST_ASSERT_EQUAL_UINT8(BMS_FAULT_CHARGER_DISOBEYS, d[0]);
    TEST_ASSERT_EQUAL_UINT8(EVENT_BMS_CHARGER, d[2]);
    TEST_ASSERT_EQUAL_UINT16(31u, (uint16_t)(d[6] | (d[7] << 8)));
    TEST_ASSERT_TRUE(any_event(EVENT_CHARGE)); /* the charge stopped */
}

static void test_no_charger_flag_is_a_bms_fault(void)
{
    start_charge();
    chargerFlags = 0x02u; /* over-temperature */
    step();
    TEST_ASSERT_TRUE(any_event(EVENT_CHARGER_FLAGS));
    steps(500u);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_HEX16(0u, out.seen);
}

/* Every cell at the target: the limit falls to 0 and 10 s later the
   charge is complete, a BMS_Status bit, with the request stop. */
static void test_charge_complete_is_a_bms_status_bit(void)
{
    start_charge();
    TEST_ASSERT_EQUAL_UINT8(0u, status().charge_complete);
    for (size_t i = 0; i < BQ_SWEEP_CELLS; i++) {
        sweep.cell_mv[i] = 4100u;
    }
    steps((BMS_CHARGE_END_MS + 1000u) / STEP_MS);
    TEST_ASSERT_EQUAL_UINT8(1u, status().charge_complete);
    TEST_ASSERT_EQUAL_UINT8(BMS_STATE_CHARGE, out.state);
    TEST_ASSERT_TRUE(out.bms_ok);
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE,
                            step_until_request().control);
}

/* Module and cell count from 0; voltages in 0.1 mV. */
static void test_bms_pack_voltages_names_the_highest_and_lowest_cell(void)
{
    sweep.cell_mv[9] = 3800u;   /* module 1, cell 1 */
    sweep.cell_mv[127] = 3600u; /* module 15, cell 7 */
    struct can_control_bms_pack_voltages_t v;
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_pack_voltages_unpack(&v, step_until(0x210u), 8u));
    TEST_ASSERT_EQUAL_UINT16(38000u, v.max_cell_voltage);
    TEST_ASSERT_EQUAL_UINT16(36000u, v.min_cell_voltage);
    TEST_ASSERT_EQUAL_UINT16(37000u, v.avg_cell_voltage);
    TEST_ASSERT_EQUAL_UINT8(1u, v.max_cell_module);
    TEST_ASSERT_EQUAL_UINT8(1u, v.max_cell_number);
    TEST_ASSERT_EQUAL_UINT8(15u, v.min_cell_module);
    TEST_ASSERT_EQUAL_UINT8(7u, v.min_cell_number);
}

/* Temperatures in 0.1 C through the sensor table; the Energy Meter's
   0x60D cannot carry below 0 C, so a colder cell goes as 0. */
static void test_the_temperature_frames_name_the_hottest_and_coldest_sensor(void)
{
    int16_t hot = 0;
    int16_t cold = 0;
    TEST_ASSERT_TRUE(cell_temp_from_mv(1700u, &hot));
    TEST_ASSERT_TRUE(cell_temp_from_mv(2400u, &cold));
    TEST_ASSERT_LESS_THAN_INT16(0, cold);
    sweep.temp_mv[20] = 1700u; /* module 2, sensor 4 */
    sweep.temp_mv[3] = 2400u;  /* module 0, sensor 3 */

    struct can_control_bms_pack_temps_t t;
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_pack_temps_unpack(&t, step_until(0x211u), 8u));
    TEST_ASSERT_EQUAL_INT16(hot, t.max_temp);
    TEST_ASSERT_EQUAL_INT16(cold, t.min_temp);
    TEST_ASSERT_EQUAL_UINT8(2u, t.max_temp_module);
    TEST_ASSERT_EQUAL_UINT8(4u, t.max_temp_sensor);
    TEST_ASSERT_EQUAL_UINT8(0u, t.min_temp_module);
    TEST_ASSERT_EQUAL_UINT8(3u, t.min_temp_sensor);

    struct can_control_em_cell_temps_t em;
    TEST_ASSERT_EQUAL_INT(0, can_control_em_cell_temps_unpack(&em, step_until(0x60Du), 8u));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)hot, em.max_cell_temp);
    TEST_ASSERT_EQUAL_UINT16(0u, em.min_cell_temp);
}

static void test_a_uniform_pack_averages_to_its_one_temperature(void)
{
    int16_t ok = 0;
    TEST_ASSERT_TRUE(cell_temp_from_mv(TEMP_OK_MV, &ok));
    struct can_control_bms_pack_temps_t t;
    TEST_ASSERT_EQUAL_INT(0, can_control_bms_pack_temps_unpack(&t, step_until(0x211u), 8u));
    TEST_ASSERT_EQUAL_INT16(ok, t.avg_temp);
    struct can_control_em_cell_temps_t em;
    TEST_ASSERT_EQUAL_INT(0, can_control_em_cell_temps_unpack(&em, step_until(0x60Du), 8u));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)ok, em.avg_cell_temp);
}

/* ADR 0018: "The under-voltage fault stays on raw loaded voltage at
   2.505 V, so a wrong resistance parameter cannot hide a low cell." 300 A
   and the largest resistance put 2.505 V back to over 3 V for the taper,
   and the fault still comes on the tenth Sweep. */
static void test_the_under_voltage_fault_stays_on_the_raw_voltage(void)
{
    derating(1200u, 200u);
    in.pack_current = (bms_current_t){ .deci_a = 3000, .valid = true };
    power_up();
    sweep.cell_mv[36] = BMS_UV_MV;
    assert_raised_on_sweep(BMS_UV_SWEEPS, BMS_FAULT_UNDER_VOLTAGE);
    TEST_ASSERT_EQUAL_UINT16(0u, power().discharge_limit);
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
    RUN_TEST(test_a_cell_under_the_low_voltage_limit_is_a_status_bit_and_no_fault);
    RUN_TEST(test_the_low_voltage_limit_follows_the_voltage_tapers_full_point);
    RUN_TEST(test_a_control_bus_failure_never_takes_bms_ok_low);
    RUN_TEST(test_overcurrent_is_never_set);
    RUN_TEST(test_each_fault_is_one_event_on_the_step_it_is_raised);
    RUN_TEST(test_every_reset_but_power_on_is_held);
    RUN_TEST(test_a_power_on_reset_is_not_held_whatever_else_is_set);
    RUN_TEST(test_no_reset_is_held_under_the_debugger);
    RUN_TEST(test_the_held_fault_never_ends);
    RUN_TEST(test_bms_ok_never_rises_while_held);
    RUN_TEST(test_bms_status_goes_every_step_and_passes_its_e2e_check);
    RUN_TEST(test_the_open_wire_check_runs_on_the_first_sweep_then_once_a_second);
    RUN_TEST(test_the_open_wire_check_runs_only_after_a_sweep);
    RUN_TEST(test_bms_ok_never_rises_before_an_open_wire_check_has_passed);
    RUN_TEST(test_two_failed_open_wire_checks_in_a_row_are_the_open_sense_wire_fault);
    RUN_TEST(test_a_device_that_did_not_finish_is_named_in_the_event);
    RUN_TEST(test_one_failed_check_then_a_pass_is_no_fault);
    RUN_TEST(test_a_check_that_could_not_judge_neither_counts_nor_clears);
    RUN_TEST(test_an_open_wire_takes_bms_ok_low_within_2_5_s);
    RUN_TEST(test_the_open_wire_fault_ends_once_checks_pass_again);
    RUN_TEST(test_nfault_low_is_the_cell_monitor_fault_at_once);
    RUN_TEST(test_a_device_reporting_its_own_fault_is_the_cell_monitor_fault);
    RUN_TEST(test_bms_ok_does_not_rise_over_a_fault_left_from_the_wake);
    RUN_TEST(test_nfault_low_while_the_chain_wakes_is_no_fault);
    RUN_TEST(test_a_bms_ok_read_back_wrong_for_5_sweeps_is_the_internal_fault);
    RUN_TEST(test_a_read_back_high_while_commanded_low_is_the_internal_fault);
    RUN_TEST(test_the_read_back_check_counts_sweeps_not_steps);
    RUN_TEST(test_a_comparator_trip_ends_a_second_after_nfault_rises);
    RUN_TEST(test_nfault_disagreeing_with_the_bridge_register_for_5_sweeps_is_internal);
    RUN_TEST(test_nfault_low_with_a_clear_register_is_internal_too);
    RUN_TEST(test_the_nfault_cross_check_waits_for_the_register);
    RUN_TEST(test_bms_ok_low_for_500_ms_with_the_sense_energised_is_internal);
    RUN_TEST(test_a_shutdown_circuit_that_opens_inside_500_ms_is_no_fault);
    RUN_TEST(test_a_timing_fault_is_the_internal_fault_at_once);
    RUN_TEST(test_a_timing_fault_recovers_after_exactly_1_s_clean);
    RUN_TEST(test_a_further_timing_fault_starts_the_clean_second_again);
    RUN_TEST(test_every_five_steps_send_status_and_power_each_step_and_each_slow_frame_once);
    RUN_TEST(test_the_discharge_limit_is_zero_until_bms_ok_is_up);
    RUN_TEST(test_hearing_the_charger_is_charge_mode_until_a_power_cycle);
    RUN_TEST(test_in_charge_the_request_goes_once_a_second_in_the_0x202_slot);
    RUN_TEST(test_balancing_status_goes_once_a_second_in_charge_mode_only);
    RUN_TEST(test_balancing_stops_in_the_step_the_sense_opens);
    RUN_TEST(test_balancing_stops_in_the_step_a_bms_fault_takes_bms_ok_low);
    RUN_TEST(test_the_charge_sees_only_the_settled_sweep);
    RUN_TEST(test_a_stopped_balancing_is_a_status_bit_and_no_bms_fault);
    RUN_TEST(test_no_charger_request_is_ever_sent_outside_charge);
    RUN_TEST(test_no_charger_request_is_sent_in_fault_held);
    RUN_TEST(test_a_silent_charger_gets_stop_every_second_and_its_return_restarts_the_charge);
    RUN_TEST(test_a_charge_through_the_step_asks_for_the_charge_limit);
    RUN_TEST(test_charger_disobeys_is_a_bms_fault);
    RUN_TEST(test_no_charger_flag_is_a_bms_fault);
    RUN_TEST(test_charge_complete_is_a_bms_status_bit);
    RUN_TEST(test_bms_power_carries_the_limit_the_current_and_the_pack_voltage);
    RUN_TEST(test_an_invalid_pack_current_goes_as_zero);
    RUN_TEST(test_a_weak_cell_lowers_the_limit_in_bms_power);
    RUN_TEST(test_the_inverter_gets_the_limit_in_whole_amps_and_no_charge);
    RUN_TEST(test_the_inverter_limit_rounds_up_to_the_whole_amp);
    RUN_TEST(test_the_inverter_limit_is_zero_until_bms_ok_is_up);
    RUN_TEST(test_bms_limits_says_no_charge_in_drive);
    RUN_TEST(test_bms_pack_voltages_names_the_highest_and_lowest_cell);
    RUN_TEST(test_the_temperature_frames_name_the_hottest_and_coldest_sensor);
    RUN_TEST(test_a_uniform_pack_averages_to_its_one_temperature);
    RUN_TEST(test_the_under_voltage_fault_stays_on_the_raw_voltage);
    return UNITY_END();
}
