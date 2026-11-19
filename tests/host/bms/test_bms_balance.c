/*
 * Host tests for balancing (App/bms/logic/Inc/bms_balance.h; issue 68;
 * ADR 0020; spec #36, "BMS image").
 *
 * - "Balancing, Charge mode only: firmware picks cells from a sweep taken
 *   with every FET off and writes the FET set once a second, alternating
 *   odd and even cells so at most 4 of a board's 8 bleed and no neighbours
 *   are on. About 900 ms bleeding, then a pause, one settled sweep and the
 *   open-wire check; only that sweep feeds the balancing choice, the charge
 *   taper and the end-of-charge test. Starts at a Cell spread over 10 mV,
 *   bleeds cells more than 5 mV above the lowest, done at 5 mV. The
 *   Balancing gate is 100 mV under the Charge target and latches for the
 *   charge."
 * - "Balancing interlock: state CHARGE, sense closed, BMS_OK high with no
 *   BMS fault, charger heard within 5 s. Any false sends the stop in the
 *   same sweep; restart needs all true for 3 s. Each bleeding channel is
 *   armed with the chip's 10 s timer and re-armed every second." EV.8.3.3:
 *   balancing must stop when the Shutdown Circuit (or the Charging
 *   Shutdown Circuit) is open; about 40 ms (user story 20).
 * - "Balancing limits: pause a board at 95 C die temperature (resume 85 C);
 *   pause all at 45 C cell temperature; stop for the session if the lowest
 *   cell is more than 200 mV under the target or 12 hours after Charge
 *   complete. A failed FET check, a rejected configuration or
 *   non-convergence stops balancing and is logged."
 * - Every FET commanded off in STARTUP, DRIVE, FAULT and FAULT_HELD.
 *
 * The harness plays bms_control.h's part: a Sweep every 20 ms step, the
 * open-wire check once a second on a step with a Sweep unless balancing
 * holds it, and each command carried out by a chain that answers as told.
 */
#include <string.h>

#include "bms_balance.h"
#include "event_codes.h"
#include "unity.h"

#define STEP_MS   20u
#define TARGET_MV 4100u
#define MV_25C    1860u /* the Enepaq sensor at 25 C */
#define MV_45C    1630u
#define MV_44C    1640u
#define MV_43C    1650u

static bms_balance_t b;
static bms_balance_in_t in;
static bms_balance_out_t out;
static bq_sweep_t sweep;
static bq_balance_result_t result;
static uint32_t now;
static bool checked;
static uint32_t lastCheck;
static bool checkRan; /* the last step ran the open-wire check */

/* How the chain answers the next command. */
static bool linkOk;
static int16_t dieDc[16];
static uint16_t rejected;
static bool failSets; /* the link fails every set */
static bq_fet_check_verdict_t fetVerdict;

/* Every command sent, in order, since the counts were cleared. */
static size_t stops, sets, fetOns, fetCompares, fetReads;
static bq_balance_cmd_t lastSet;
static size_t eventsSeen;
static event_balancing_t lastEvent;

static void cells_all(uint16_t mv)
{
    for (size_t i = 0u; i < BQ_SWEEP_CELLS; i++) {
        sweep.cell_mv[i] = mv;
    }
}

static void temps_all(uint16_t mv)
{
    for (size_t i = 0u; i < BQ_SWEEP_CELLS; i++) {
        sweep.temp_mv[i] = mv;
    }
}

static void clear_counts(void)
{
    stops = sets = fetOns = fetCompares = fetReads = 0u;
    eventsSeen = 0u;
}

void setUp(void)
{
    memset(&sweep, 0, sizeof sweep);
    sweep.devices_read = 0xFFFFu;
    cells_all(4050u); /* at the gate */
    temps_all(MV_25C);
    memset(&in, 0, sizeof in);
    in.charge_mode = true;
    in.state_charge = true;
    in.sdc_closed = true;
    in.bms_ok = true;
    in.charger_heard = true;
    in.swept = true;
    in.sweep = &sweep;
    in.result = &result;
    now = 1000u;
    checked = false;
    lastCheck = 0u;
    linkOk = true;
    for (size_t d = 0u; d < 16u; d++) {
        dieDc[d] = 400;
    }
    rejected = 0u;
    failSets = false;
    fetVerdict = BQ_FET_CHECK_PASS;
    memset(&out, 0, sizeof out);
    clear_counts();
    bms_balance_init(&b, TARGET_MV);
}

void tearDown(void) {}

static void unpack_event(const uint8_t *d)
{
    lastEvent = (event_balancing_t){
        .from = d[0],
        .to = d[1],
        .reason = d[2],
        .cells = {
            .highest_mv = (uint16_t)(d[4] | (d[5] << 8)),
            .highest = (uint16_t)(d[6] | (d[7] << 8)),
            .lowest_mv = (uint16_t)(d[8] | (d[9] << 8)),
            .lowest = (uint16_t)(d[10] | (d[11] << 8)),
        },
        .spread_mv = (uint16_t)(d[12] | (d[13] << 8)),
        .detail = (uint16_t)(d[14] | (d[15] << 8)),
    };
}

/* One 20 ms step, as bms_control runs it, then the command carried out. */
static void step(void)
{
    in.now_ms = now;
    in.check_due = in.swept && (!checked || (uint32_t)(now - lastCheck) >= 1000u);
    in.checked = checked;
    in.last_check_ms = lastCheck;
    bms_balance_step(&b, &in, &out);

    for (size_t i = 0u; i < out.event_count; i++) {
        TEST_ASSERT_EQUAL_UINT16(EVENT_BALANCING, out.events[i].code);
        unpack_event(out.events[i].data);
        eventsSeen++;
    }

    checkRan = in.check_due && !out.hold_check && out.command.op == BQ_BALANCE_NONE;
    if (checkRan) {
        checked = true;
        lastCheck = now;
    }

    memset(&result, 0, sizeof result);
    result.ok = linkOk && !(failSets && out.command.op == BQ_BALANCE_SET);
    result.fet = BQ_FET_CHECK_UNKNOWN;
    in.ran = out.command.op != BQ_BALANCE_NONE;
    switch (out.command.op) {
    case BQ_BALANCE_STOP:
        stops++;
        result.devices_read = linkOk ? 0xFFFFu : 0u;
        memcpy(result.die_dc, dieDc, sizeof dieDc);
        break;
    case BQ_BALANCE_SET:
        sets++;
        lastSet = out.command;
        result.rejected = rejected;
        break;
    case BQ_BALANCE_FET_ON:
        fetOns++;
        break;
    case BQ_BALANCE_FET_COMPARE:
        fetCompares++;
        break;
    case BQ_BALANCE_FET_READ:
        fetReads++;
        result.fet = fetVerdict;
        result.fet_device = (fetVerdict == BQ_FET_CHECK_FAIL) ? 3u : 0u;
        result.fet_cell = (fetVerdict == BQ_FET_CHECK_FAIL) ? 20u : 0u;
        break;
    default:
        break;
    }
    now += STEP_MS;
}

static void steps(uint32_t ms)
{
    for (uint32_t t = 0u; t < ms; t += STEP_MS) {
        step();
    }
}

/* Steps until the step just run returned op; fails rather than hangs. */
static void step_until(bq_balance_op_t op)
{
    for (int i = 0; i < 2000; i++) {
        step();
        if (out.command.op == op) {
            return;
        }
    }
    TEST_FAIL_MESSAGE("never came");
}

/* Steps until the state is state. */
static void step_until_state(uint8_t state)
{
    for (int i = 0; i < 5000 && out.state != state; i++) {
        step();
    }
    TEST_ASSERT_EQUAL_UINT8(state, out.state);
}

static bool any_on(void)
{
    for (size_t d = 0u; d < 16u; d++) {
        if (out.on[d] != 0u) {
            return true;
        }
    }
    return false;
}

/* A pack with a spread: cell 1 of device 1 low, every other cell 20 mV
   above it. */
static void spread_pack(void)
{
    cells_all(4070u);
    sweep.cell_mv[0] = 4050u;
}

/* From the start to the first set written: gate, decision, FET check,
   the pause, the next decision, the set. */
static void run_to_first_set(void)
{
    spread_pack();
    step_until(BQ_BALANCE_SET);
}

/* ---- off and the gate ---------------------------------------------------- */

static void test_outside_charge_mode_balancing_is_off_and_sends_nothing(void)
{
    in.charge_mode = false;
    in.state_charge = false;
    spread_pack();
    steps(5000u);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_OFF, out.state);
    TEST_ASSERT_EQUAL_UINT(0u, stops + sets + fetOns);
    TEST_ASSERT_TRUE(out.settled); /* no cell bleeds: every Sweep is settled */
}

static void test_under_the_gate_balancing_waits_whatever_the_spread(void)
{
    cells_all(3999u); /* 101 mV under the 4.100 V target */
    sweep.cell_mv[5] = 3900u;
    steps(5000u);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_WAITING, out.state);
    TEST_ASSERT_EQUAL_UINT(0u, sets + fetOns);
    TEST_ASSERT_EQUAL_UINT16(99u, out.spread_mv);
}

static void test_the_gate_is_100_mv_under_the_target_and_latches_for_the_charge(void)
{
    cells_all(3990u);
    sweep.cell_mv[7] = 4000u; /* the highest cell reaches 4.000 V */
    step();
    TEST_ASSERT_TRUE(b.gate);
    TEST_ASSERT_NOT_EQUAL(BMS_BALANCE_WAITING, out.state);

    cells_all(3950u); /* the cells fall back */
    steps(3000u);
    TEST_ASSERT_TRUE(b.gate);
    TEST_ASSERT_NOT_EQUAL(BMS_BALANCE_WAITING, out.state);
}

/* ---- sessions and selection --------------------------------------------- */

static void test_a_spread_of_10_mv_starts_nothing_and_11_starts_a_session(void)
{
    cells_all(4060u);
    sweep.cell_mv[9] = 4050u; /* spread 10 mV */
    steps(5000u);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_DONE, out.state);
    TEST_ASSERT_EQUAL_UINT(0u, sets + fetOns);

    sweep.cell_mv[9] = 4049u; /* 11 mV */
    step_until_state(BMS_BALANCE_BALANCING);
    TEST_ASSERT_EQUAL_UINT16(11u, out.spread_mv);
}

/* ADR 0020: the FET check runs once per charge before the first bleed:
   odd FETs, then even, each on, compared and read. */
static void test_the_fet_check_runs_once_before_the_first_bleed(void)
{
    spread_pack();
    step_until(BQ_BALANCE_FET_ON);
    TEST_ASSERT_EQUAL_HEX8(BMS_BALANCE_ODD_CELLS, out.command.fet_cells);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
    const uint32_t on = now;
    step_until(BQ_BALANCE_FET_COMPARE);
    TEST_ASSERT_TRUE(now - on >= BMS_BALANCE_FET_SETTLE_MS);
    step_until(BQ_BALANCE_FET_READ);
    TEST_ASSERT_EQUAL_HEX8(BMS_BALANCE_ODD_CELLS, out.command.fet_cells);
    step_until(BQ_BALANCE_FET_ON);
    TEST_ASSERT_EQUAL_HEX8(BMS_BALANCE_EVEN_CELLS, out.command.fet_cells);
    step_until(BQ_BALANCE_FET_READ);
    TEST_ASSERT_EQUAL_HEX8(BMS_BALANCE_EVEN_CELLS, out.command.fet_cells);
    TEST_ASSERT_EQUAL_UINT(0u, sets);

    step_until(BQ_BALANCE_SET);
    steps(10000u);
    TEST_ASSERT_EQUAL_UINT(2u, fetOns); /* never again this charge */
}

/* The open-wire check never runs while the FET check is under way: both
   use the cell monitor's comparison. */
static void test_the_open_wire_check_waits_for_the_fet_check(void)
{
    spread_pack();
    step_until(BQ_BALANCE_FET_ON);
    while (b.fet_stage != BMS_FET_CHECK_IDLE || b.fet_cells != BMS_BALANCE_EVEN_CELLS) {
        TEST_ASSERT_TRUE(out.hold_check);
        TEST_ASSERT_FALSE(checkRan);
        step();
    }
}

static void test_every_cell_more_than_5_mv_above_the_lowest_bleeds_odd_and_even_in_turn(void)
{
    cells_all(4050u);
    sweep.cell_mv[0] = 4040u; /* device 1 cell 1: the lowest */
    sweep.cell_mv[1] = 4045u; /* exactly 5 mV above: no */
    for (size_t k = 0u; k < 8u; k++) {
        sweep.cell_mv[8u + k] = 4060u; /* all of device 2 */
    }
    step_until(BQ_BALANCE_SET);
    const uint8_t first = lastSet.cells[1];
    TEST_ASSERT_TRUE(first == BMS_BALANCE_ODD_CELLS || first == BMS_BALANCE_EVEN_CELLS);
    /* Device 1: cells 3 to 8, this second's parity. */
    TEST_ASSERT_EQUAL_HEX8(first & 0xFCu, lastSet.cells[0]);

    step_until(BQ_BALANCE_SET);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)~first, lastSet.cells[1]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)~first & 0xFCu, lastSet.cells[0]);
}

/* No two neighbours on, at most 4 of 8, on every board and every second. */
static void test_no_board_ever_has_neighbours_on_or_more_than_four(void)
{
    cells_all(4080u);
    sweep.cell_mv[64] = 4050u;
    for (int second = 0; second < 6; second++) {
        step_until(BQ_BALANCE_SET);
        for (size_t d = 0u; d < 16u; d++) {
            const uint8_t m = lastSet.cells[d];
            TEST_ASSERT_EQUAL_HEX8(0u, m & (uint8_t)(m << 1));
            uint8_t n = 0u;
            for (uint8_t k = 0u; k < 8u; k++) {
                n += (m >> k) & 1u;
            }
            TEST_ASSERT_TRUE(n <= 4u);
        }
    }
}

static void test_a_board_with_cells_of_one_parity_only_bleeds_them_every_second(void)
{
    cells_all(4050u);
    sweep.cell_mv[2] = 4061u; /* device 1 cell 3 */
    sweep.cell_mv[5] = 4061u; /* and cell 6: odd and even */
    sweep.cell_mv[8 + 4] = 4061u; /* device 2 cell 5 only */
    for (int second = 0; second < 4; second++) {
        step_until(BQ_BALANCE_SET);
        TEST_ASSERT_EQUAL_HEX8(0x10u, lastSet.cells[1]);
        TEST_ASSERT_TRUE(lastSet.cells[0] == 0x04u || lastSet.cells[0] == 0x20u);
    }
}

/* ---- the second: set, bleed, pause, settled Sweep ----------------------- */

/* Each second the set is written again: the chip's 10 s timer re-armed. */
static void test_the_set_is_written_once_a_second_the_step_after_the_check(void)
{
    run_to_first_set();
    const uint32_t first = now;
    clear_counts();
    step_until(BQ_BALANCE_SET);
    TEST_ASSERT_EQUAL_UINT32(1000u, now - first);
    TEST_ASSERT_EQUAL_UINT(1u, stops); /* the pause between */
    /* The step before the set ran the check. */
    TEST_ASSERT_EQUAL_UINT32(now - 2u * STEP_MS, lastCheck);
}

/* About 900 ms bleeding, then the pause: every FET off 60 ms before the
   settled Sweep that the check and the decision take. */
static void test_the_pause_turns_every_fet_off_60_ms_before_the_settled_sweep(void)
{
    run_to_first_set();
    const uint32_t setAt = now - STEP_MS;
    step();
    TEST_ASSERT_TRUE(any_on());
    TEST_ASSERT_FALSE(out.settled);

    step_until(BQ_BALANCE_STOP);
    const uint32_t stopAt = now - STEP_MS;
    TEST_ASSERT_EQUAL_UINT32(BMS_BALANCE_PERIOD_MS - BMS_BALANCE_PAUSE_MS,
                             stopAt - lastCheck);
    TEST_ASSERT_TRUE(stopAt - setAt >= 900u);

    step(); /* 20 ms after the stop */
    TEST_ASSERT_FALSE(any_on());
    TEST_ASSERT_FALSE(out.settled);
    step();
    TEST_ASSERT_FALSE(out.settled);
    step(); /* 60 ms: the check's Sweep */
    TEST_ASSERT_TRUE(out.settled);
    TEST_ASSERT_TRUE(checkRan);
}

/* Only the paused Sweep feeds the choice: a cell that reads high only while
   bleeding (the tap-wire error) changes nothing. */
static void test_only_the_settled_sweep_feeds_the_choice(void)
{
    run_to_first_set();
    step();
    TEST_ASSERT_FALSE(out.settled);
    sweep.cell_mv[0] = 4000u; /* would be a LOW_CELL stop if judged */
    step();
    step();
    TEST_ASSERT_NOT_EQUAL(BMS_BALANCE_STOPPED, out.state);
    TEST_ASSERT_EQUAL_UINT16(20u, out.spread_mv);
}

static void test_a_session_is_done_at_5_mv(void)
{
    run_to_first_set();
    cells_all(4070u);
    sweep.cell_mv[0] = 4065u; /* 5 mV */
    step_until_state(BMS_BALANCE_DONE);
    TEST_ASSERT_EQUAL_UINT16(5u, out.spread_mv);
    step(); /* no set after it */
    clear_counts();
    steps(3000u);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
    TEST_ASSERT_FALSE(any_on());

    /* 6 to 10 mV does not start another; over 10 does. */
    sweep.cell_mv[0] = 4060u;
    steps(3000u);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_DONE, out.state);
    sweep.cell_mv[0] = 4059u;
    step_until(BQ_BALANCE_SET);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_BALANCING, out.state);
}

static void test_a_settled_sweep_missing_a_cell_chooses_nothing(void)
{
    run_to_first_set();
    sweep.devices_read = 0xFFFEu;
    clear_counts();
    steps(3000u);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
    TEST_ASSERT_FALSE(any_on());
}

/* ---- the interlock (EV.8.3.3) ------------------------------------------- */

/* Any condition false while bleeding: the stop goes in the same step. */
static void check_interlock(void (*breaks)(void), void (*mends)(void), uint8_t reason)
{
    setUp();
    run_to_first_set();
    step();
    TEST_ASSERT_TRUE(any_on());

    breaks();
    step();
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.command.op);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_PAUSED, out.state);
    TEST_ASSERT_EQUAL_UINT8(reason, out.reason);
    TEST_ASSERT_EQUAL_UINT8(reason, lastEvent.reason);
    step();
    TEST_ASSERT_FALSE(any_on());

    /* Nothing while it stays false, and for 3 s after it is mended. */
    clear_counts();
    steps(5000u);
    mends();
    steps(BMS_BALANCE_RESTART_MS - STEP_MS);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_PAUSED, out.state);
    step_until(BQ_BALANCE_SET);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_BALANCING, out.state);
}

static void sense_open(void) { in.sdc_closed = false; }
static void sense_closed(void) { in.sdc_closed = true; }
static void bms_ok_low(void) { in.bms_ok = false; }
static void bms_ok_high(void) { in.bms_ok = true; }
static void fault(void) { in.fault_active = true; }
static void no_fault(void) { in.fault_active = false; }
static void not_charge(void) { in.state_charge = false; }
static void charge(void) { in.state_charge = true; }
static void silent(void) { in.charger_heard = false; }
static void heard(void) { in.charger_heard = true; }

static void test_the_sense_opening_stops_balancing_in_the_same_step(void)
{
    check_interlock(sense_open, sense_closed, BMS_BALANCE_SENSE_OPEN);
}

static void test_bms_ok_low_stops_balancing_in_the_same_step(void)
{
    check_interlock(bms_ok_low, bms_ok_high, BMS_BALANCE_BMS_FAULT);
}

static void test_a_bms_fault_stops_balancing_in_the_same_step(void)
{
    check_interlock(fault, no_fault, BMS_BALANCE_BMS_FAULT);
}

/* FAULT and FAULT_HELD (and STARTUP) are not the state CHARGE. */
static void test_leaving_the_charge_state_stops_balancing_in_the_same_step(void)
{
    check_interlock(not_charge, charge, BMS_BALANCE_BMS_FAULT);
}

/* A silent charger is a pause, like the others. */
static void test_a_silent_charger_pauses_balancing(void)
{
    check_interlock(silent, heard, BMS_BALANCE_CHARGER_SILENT);
}

static void test_a_break_during_the_3_s_starts_it_again(void)
{
    run_to_first_set();
    in.sdc_closed = false;
    step();
    in.sdc_closed = true;
    steps(2000u);
    in.sdc_closed = false;
    step();
    in.sdc_closed = true;
    clear_counts();
    steps(BMS_BALANCE_RESTART_MS - STEP_MS);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
}

/* The sense opening during the FET check turns its FETs off at once. */
static void test_the_sense_opening_during_the_fet_check_stops_it(void)
{
    spread_pack();
    step_until(BQ_BALANCE_FET_ON);
    step();
    in.sdc_closed = false;
    step();
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.command.op);
    step(); /* the check given up: the open-wire check may run */
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_NONE, out.command.op);
    TEST_ASSERT_FALSE(out.hold_check);
}

/* A stop the link did not carry goes again on the next step. */
static void test_a_stop_that_failed_goes_again(void)
{
    run_to_first_set();
    step();
    in.sdc_closed = false;
    linkOk = false;
    step();
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.command.op);
    step();
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.command.op);
    linkOk = true;
    step();
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.command.op);
    step();
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_NONE, out.command.op);
    TEST_ASSERT_FALSE(any_on());
}

/* A set the link did not carry may have left FETs on: it is stopped. */
static void test_a_set_that_failed_is_stopped(void)
{
    spread_pack();
    failSets = true;
    step_until(BQ_BALANCE_SET);
    failSets = false;
    step();
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.command.op);
    TEST_ASSERT_FALSE(out.settled);
}

/* ---- pauses ------------------------------------------------------------- */

static void test_a_board_pauses_at_95_c_die_temperature_and_resumes_at_85_c(void)
{
    cells_all(4070u);
    sweep.cell_mv[0] = 4050u;
    run_to_first_set();
    TEST_ASSERT_NOT_EQUAL(0u, lastSet.cells[2]);

    dieDc[2] = 950;
    step_until(BQ_BALANCE_SET);
    TEST_ASSERT_EQUAL_HEX8(0u, lastSet.cells[2]);
    TEST_ASSERT_NOT_EQUAL(0u, lastSet.cells[3]);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_BALANCING, out.state);

    dieDc[2] = 851;
    step_until(BQ_BALANCE_SET);
    TEST_ASSERT_EQUAL_HEX8(0u, lastSet.cells[2]);
    dieDc[2] = 850;
    step_until(BQ_BALANCE_SET);
    TEST_ASSERT_NOT_EQUAL(0u, lastSet.cells[2]);
}

static void test_every_hot_board_is_paused_board_hot(void)
{
    run_to_first_set();
    for (size_t d = 0u; d < 16u; d++) {
        dieDc[d] = 960;
    }
    step_until_state(BMS_BALANCE_PAUSED);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_BOARD_HOT, out.reason);
    clear_counts();
    steps(3000u);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
}

static void test_every_board_pauses_at_45_c_cell_temperature(void)
{
    run_to_first_set();
    sweep.temp_mv[77] = MV_45C;
    step_until_state(BMS_BALANCE_PAUSED);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_CELLS_HOT, out.reason);
    clear_counts();
    sweep.temp_mv[77] = MV_44C;
    steps(3000u);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
    sweep.temp_mv[77] = MV_43C + 1u; /* just under 43 C */
    step_until(BQ_BALANCE_SET);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_BALANCING, out.state);
}

/* ---- stops -------------------------------------------------------------- */

static void test_the_lowest_cell_more_than_200_mv_under_the_target_stops_it(void)
{
    run_to_first_set();
    sweep.cell_mv[0] = 3900u; /* exactly 200 mV under: not yet */
    steps(3000u);
    TEST_ASSERT_NOT_EQUAL(BMS_BALANCE_STOPPED, out.state);
    sweep.cell_mv[0] = 3899u;
    step_until_state(BMS_BALANCE_STOPPED);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_LOW_CELL, out.reason);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_LOW_CELL, lastEvent.reason);
    TEST_ASSERT_EQUAL_UINT16(3899u, lastEvent.cells.lowest_mv);
    TEST_ASSERT_EQUAL_UINT16(1u, lastEvent.cells.lowest);
    TEST_ASSERT_TRUE(bms_balance_stopped(&b));

    /* For good: no more sets, and every FET off. */
    sweep.cell_mv[0] = 4050u;
    clear_counts();
    steps(5000u);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
    TEST_ASSERT_FALSE(any_on());
}

static void test_a_failed_fet_check_stops_balancing_with_the_fet_named(void)
{
    spread_pack();
    fetVerdict = BQ_FET_CHECK_FAIL;
    step_until_state(BMS_BALANCE_STOPPED);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_FET_CHECK_FAILED, out.reason);
    TEST_ASSERT_EQUAL_HEX16(0x0304u, lastEvent.detail); /* device 3, FET 4 */
    clear_counts();
    steps(5000u);
    TEST_ASSERT_EQUAL_UINT(0u, sets + fetOns);
}

/* A FET check not read whole is run again at the next decision. */
static void test_a_fet_check_not_read_whole_is_run_again(void)
{
    spread_pack();
    fetVerdict = BQ_FET_CHECK_UNKNOWN;
    step_until(BQ_BALANCE_FET_READ);
    step();
    fetVerdict = BQ_FET_CHECK_PASS;
    step_until(BQ_BALANCE_FET_ON);
    TEST_ASSERT_EQUAL_HEX8(BMS_BALANCE_ODD_CELLS, out.command.fet_cells);
    TEST_ASSERT_EQUAL_UINT(0u, sets);
}

static void test_a_rejected_configuration_stops_balancing(void)
{
    rejected = 0x0010u;
    run_to_first_set();
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_STOPPED, out.state);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_CONFIG_REJECTED, out.reason);
    TEST_ASSERT_EQUAL_HEX16(0x0010u, lastEvent.detail);
    TEST_ASSERT_EQUAL_INT(BQ_BALANCE_STOP, out.command.op);
}

/* Balancing carries on after Charge complete for at most 12 hours,
   counted through pauses; not done by then is no convergence. */
static void test_12_hours_after_charge_complete_an_unfinished_balance_stops(void)
{
    run_to_first_set();
    in.charge_complete = true;
    step();
    in.sdc_closed = false; /* a pause counts too */
    steps(60000u);
    in.sdc_closed = true;
    now += BMS_BALANCE_TIME_LIMIT_MS - 60000u - 2u * STEP_MS; /* the time passes */
    step();
    TEST_ASSERT_NOT_EQUAL(BMS_BALANCE_STOPPED, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_STOPPED, out.state);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_TIME_LIMIT, out.reason);
}

static void test_a_balance_done_by_the_12_hours_stays_done(void)
{
    cells_all(4070u);
    in.charge_complete = true;
    steps(5000u);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_DONE, out.state);
    now += BMS_BALANCE_TIME_LIMIT_MS;
    step();
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_DONE, out.state);
    TEST_ASSERT_FALSE(bms_balance_stopped(&b));

    spread_pack(); /* no session after the time is up */
    clear_counts();
    steps(5000u);
    TEST_ASSERT_EQUAL_UINT(0u, sets + fetOns);
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_DONE, out.state);
}

/* ---- Events and names --------------------------------------------------- */

static void test_each_change_of_state_is_an_event_and_nothing_else_is(void)
{
    run_to_first_set();
    TEST_ASSERT_EQUAL_UINT8(BMS_BALANCE_BALANCING, lastEvent.to);
    TEST_ASSERT_EQUAL_UINT16(20u, lastEvent.spread_mv);
    clear_counts();
    steps(5000u); /* balancing on, second after second */
    TEST_ASSERT_EQUAL_UINT(0u, eventsSeen);
}

static void test_names(void)
{
    TEST_ASSERT_EQUAL_STRING("WAITING_FOR_GATE", bms_balance_state_name(BMS_BALANCE_WAITING));
    TEST_ASSERT_EQUAL_STRING("STOPPED", bms_balance_state_name(BMS_BALANCE_STOPPED));
    TEST_ASSERT_EQUAL_STRING("?", bms_balance_state_name(6u));
    TEST_ASSERT_EQUAL_STRING("CHARGER_SILENT",
                             bms_balance_reason_name(BMS_BALANCE_CHARGER_SILENT));
    TEST_ASSERT_EQUAL_STRING("CONFIG_REJECTED",
                             bms_balance_reason_name(BMS_BALANCE_CONFIG_REJECTED));
    TEST_ASSERT_EQUAL_STRING("?", bms_balance_reason_name(10u));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_outside_charge_mode_balancing_is_off_and_sends_nothing);
    RUN_TEST(test_under_the_gate_balancing_waits_whatever_the_spread);
    RUN_TEST(test_the_gate_is_100_mv_under_the_target_and_latches_for_the_charge);
    RUN_TEST(test_a_spread_of_10_mv_starts_nothing_and_11_starts_a_session);
    RUN_TEST(test_the_fet_check_runs_once_before_the_first_bleed);
    RUN_TEST(test_the_open_wire_check_waits_for_the_fet_check);
    RUN_TEST(test_every_cell_more_than_5_mv_above_the_lowest_bleeds_odd_and_even_in_turn);
    RUN_TEST(test_no_board_ever_has_neighbours_on_or_more_than_four);
    RUN_TEST(test_a_board_with_cells_of_one_parity_only_bleeds_them_every_second);
    RUN_TEST(test_the_set_is_written_once_a_second_the_step_after_the_check);
    RUN_TEST(test_the_pause_turns_every_fet_off_60_ms_before_the_settled_sweep);
    RUN_TEST(test_only_the_settled_sweep_feeds_the_choice);
    RUN_TEST(test_a_session_is_done_at_5_mv);
    RUN_TEST(test_a_settled_sweep_missing_a_cell_chooses_nothing);
    RUN_TEST(test_the_sense_opening_stops_balancing_in_the_same_step);
    RUN_TEST(test_bms_ok_low_stops_balancing_in_the_same_step);
    RUN_TEST(test_a_bms_fault_stops_balancing_in_the_same_step);
    RUN_TEST(test_leaving_the_charge_state_stops_balancing_in_the_same_step);
    RUN_TEST(test_a_silent_charger_pauses_balancing);
    RUN_TEST(test_a_break_during_the_3_s_starts_it_again);
    RUN_TEST(test_the_sense_opening_during_the_fet_check_stops_it);
    RUN_TEST(test_a_stop_that_failed_goes_again);
    RUN_TEST(test_a_set_that_failed_is_stopped);
    RUN_TEST(test_a_board_pauses_at_95_c_die_temperature_and_resumes_at_85_c);
    RUN_TEST(test_every_hot_board_is_paused_board_hot);
    RUN_TEST(test_every_board_pauses_at_45_c_cell_temperature);
    RUN_TEST(test_the_lowest_cell_more_than_200_mv_under_the_target_stops_it);
    RUN_TEST(test_a_failed_fet_check_stops_balancing_with_the_fet_named);
    RUN_TEST(test_a_fet_check_not_read_whole_is_run_again);
    RUN_TEST(test_a_rejected_configuration_stops_balancing);
    RUN_TEST(test_12_hours_after_charge_complete_an_unfinished_balance_stops);
    RUN_TEST(test_a_balance_done_by_the_12_hours_stays_done);
    RUN_TEST(test_each_change_of_state_is_an_event_and_nothing_else_is);
    RUN_TEST(test_names);
    return UNITY_END();
}
