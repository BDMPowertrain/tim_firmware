/*
 * Host tests for charge control (App/bms/logic/Inc/bms_charge.h; issue 67;
 * ADR 0009, 0019; spec #36, "BMS image").
 *
 * - Charge mode begins when the charger status frame is first heard and
 *   lasts until a power cycle.
 * - "Charge limit = the lowest of the full charge limit, a taper from
 *   50 mV under the target to zero at the target on the highest cell, and
 *   tapers to zero from 50 to 55 C and from 10 to 5 C, with the same
 *   bounded rates as the discharge limit" (200%/s falling, 10%/s rising).
 * - "Start is automatic once charger heard, no BMS fault, BMS_OK high,
 *   sense closed and charge limit above zero all hold for 3 s."
 * - "End: charge limit under 0.5 A for 10 s with the highest cell at the
 *   target"; Charge complete, no restart until a power cycle; the rested
 *   highest and lowest cell written 60 s after the stop.
 * - "Charger timeout (5 s): stay in CHARGE, keep sending stop, write an
 *   Event, restart under the start conditions when it returns."
 * - "No charger status flag is a BMS fault; each flag change writes an
 *   Event."
 * - "Charger disobeys (reported current more than 1 A over the request for
 *   5 s, or over 1 A for 5 s after a stop)."
 * - The request: "voltage 128 x the charge target, current the charge
 *   limit, control byte 0 while charging and 1 with 0 V and 0 A otherwise".
 *
 * The full charge limit here is 10.0 A, so a percentage reads straight off
 * as tenths of an amp: 50% is 50.
 */
#include <string.h>

#include "bms_charge.h"
#include "can_control.h"
#include "event_codes.h"
#include "mailbox.h"
#include "unity.h"

#define STEP_MS 20u

static const bms_charge_config_t spec = {
    .target_mv = 4100u,
    .full_limit_da = 100u, /* 10.0 A */
    .v_band_mv = 50u,
    .hot_full_dc = 500,
    .hot_zero_dc = 550,
    .cold_full_dc = 100,
    .cold_zero_dc = 50,
};

static bms_charge_t c;
static mailbox_t charger;
static bms_pack_summary_t summary;
static bms_charge_in_t in;
static bms_charge_heard_t heard;
static bms_charge_events_t events;
static uint16_t limit;
static uint32_t now;

/* The charger's status as its next frame will say it. */
static uint16_t reportedDa;
static uint8_t flags;

static void write_frame(void)
{
    const struct can_control_chg_status_t s = {
        .output_voltage = 4800u,
        .output_current = reportedDa,
        .hardware_failure = (flags >> 0) & 1u,
        .over_temperature = (flags >> 1) & 1u,
        .input_voltage_wrong = (flags >> 2) & 1u,
        .starting_state = (flags >> 3) & 1u,
        .comm_timeout = (flags >> 4) & 1u,
    };
    TEST_ASSERT_EQUAL_INT(8, can_control_chg_status_pack(charger.data, &s, 8u));
}

/* The mailbox VALID, holding a good frame: the charger heard. */
static void hear(void)
{
    charger.state = MAILBOX_VALID;
    charger.counters.good++;
    write_frame();
}

/* Timed out: a reader gets the Substitute. */
static void silence(void)
{
    charger.state = MAILBOX_INVALID;
}

static void cells(uint16_t highest_mv, uint16_t lowest_mv)
{
    summary.max_mv = highest_mv;
    summary.max_cell = 9u;
    summary.min_mv = lowest_mv;
    summary.min_cell = 100u;
}

static void temps(int16_t hottest_dc, int16_t coldest_dc)
{
    summary.max_dc = hottest_dc;
    summary.min_dc = coldest_dc;
}

void setUp(void)
{
    memset(&charger, 0, sizeof charger);
    memset(&summary, 0, sizeof summary);
    memset(&events, 0, sizeof events);
    summary.cells = 128u;
    summary.temps = 128u;
    cells(3900u, 3880u);
    temps(250, 240);
    reportedDa = 0u;
    flags = 0u;
    now = 1000u;
    in = (bms_charge_in_t){
        .swept = true,
        .summary = &summary,
        .bms_ok = true,
        .fault_active = false,
        .sdc_closed = true,
    };
    bms_charge_init(&c, &spec);
}

void tearDown(void) {}

/* One 20 ms step, as bms_control runs it. The mailbox's frame is
   refreshed each step while heard, as if from the charger. */
static void step(void)
{
    events.count = 0u;
    if (charger.state == MAILBOX_VALID) {
        write_frame();
    }
    heard = bms_charge_listen(&c, now, &charger, &events);
    in.now_ms = now;
    limit = bms_charge_step(&c, &in, &events);
    now += STEP_MS;
}

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += STEP_MS) {
        step();
    }
}

static struct can_control_chg_request_t request(void)
{
    struct can_control_chg_request_t r;
    bms_charge_request(&c, now - STEP_MS, &r);
    return r;
}

/* Steps with a request once a second, as bms_control sends it. */
static void run_requesting(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += STEP_MS) {
        step();
        if ((t % 1000u) == 0u) {
            (void)request();
        }
    }
}

/* This step's Event with that code, or NULL. */
static const bms_charge_event_t *event(uint16_t code)
{
    for (size_t i = 0; i < events.count; i++) {
        if (events.events[i].code == code) {
            return &events.events[i];
        }
    }
    return NULL;
}

static uint16_t u16_at(const uint8_t *d, size_t at)
{
    return (uint16_t)(d[at] | (d[at + 1u] << 8));
}

static uint32_t u32_at(const uint8_t *d, size_t at)
{
    return (uint32_t)u16_at(d, at) | ((uint32_t)u16_at(d, at + 2u) << 16);
}

/* The first step of Charge mode: the limit at once. */
static uint16_t first(void)
{
    hear();
    step();
    return limit;
}

/* ---- Charge mode ---- */

static void test_with_no_charger_heard_it_is_not_charge_mode_and_the_limit_is_zero(void)
{
    run_ms(10000u);
    TEST_ASSERT_FALSE(heard.charge_mode);
    TEST_ASSERT_EQUAL(BMS_CHARGE_OFF, bms_charge_state(&c));
    TEST_ASSERT_EQUAL_UINT16(0u, limit);
}

/* One good frame is enough, before the mailbox is VALID; and it lasts. */
static void test_charge_mode_begins_on_the_first_good_frame_and_lasts_through_silence(void)
{
    charger.counters.good = 1u; /* not yet VALID: two more to come */
    step();
    TEST_ASSERT_TRUE(heard.charge_mode);
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));

    hear();
    step();
    silence();
    run_ms(60000u);
    TEST_ASSERT_TRUE(heard.charge_mode);
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));
}

/* ---- The Charge limit ---- */

static void test_a_cool_pack_well_under_the_target_gets_the_full_charge_limit(void)
{
    TEST_ASSERT_EQUAL_UINT16(100u, first());
}

static void test_the_voltage_taper_runs_from_50_mv_under_the_target_to_zero_at_it(void)
{
    cells(4050u, 4000u);
    TEST_ASSERT_EQUAL_UINT16(100u, first());
    setUp();
    cells(4075u, 4000u);
    TEST_ASSERT_EQUAL_UINT16(50u, first()); /* halfway */
    setUp();
    cells(4090u, 4000u);
    TEST_ASSERT_EQUAL_UINT16(20u, first());
    setUp();
    cells(4100u, 4000u);
    TEST_ASSERT_EQUAL_UINT16(0u, first());
    setUp();
    cells(4150u, 4000u);
    TEST_ASSERT_EQUAL_UINT16(0u, first());
}

static void test_the_hot_taper_runs_from_50_c_to_zero_at_55_c(void)
{
    temps(500, 240);
    TEST_ASSERT_EQUAL_UINT16(100u, first());
    setUp();
    temps(525, 240);
    TEST_ASSERT_EQUAL_UINT16(50u, first());
    setUp();
    temps(550, 240);
    TEST_ASSERT_EQUAL_UINT16(0u, first());
}

/* The coldest sensor, not the hottest. */
static void test_the_cold_taper_runs_from_10_c_to_zero_at_5_c(void)
{
    temps(250, 100);
    TEST_ASSERT_EQUAL_UINT16(100u, first());
    setUp();
    temps(250, 75);
    TEST_ASSERT_EQUAL_UINT16(50u, first());
    setUp();
    temps(250, 50);
    TEST_ASSERT_EQUAL_UINT16(0u, first());
    setUp();
    temps(250, -100);
    TEST_ASSERT_EQUAL_UINT16(0u, first());
}

static void test_the_lowest_taper_wins(void)
{
    cells(4080u, 4000u); /* 40% */
    temps(510, 240);     /* 80% */
    TEST_ASSERT_EQUAL_UINT16(40u, first());
    setUp();
    cells(4060u, 4000u); /* 80% */
    temps(250, 70);      /* 40% */
    TEST_ASSERT_EQUAL_UINT16(40u, first());
}

/* 200%/s falling: 4% of the full limit a 20 ms step. 10%/s rising: 1% in
   100 ms, 100% in 10 s. */
static void test_the_limit_moves_at_the_discharge_limits_bounded_rates(void)
{
    TEST_ASSERT_EQUAL_UINT16(100u, first());
    cells(4100u, 4000u); /* target 0% */
    step();
    TEST_ASSERT_EQUAL_UINT16(96u, limit);
    run_ms(480u);
    TEST_ASSERT_EQUAL_UINT16(0u, limit); /* 100% gone in 500 ms */

    cells(3900u, 3880u); /* target 100% */
    run_ms(100u);
    TEST_ASSERT_EQUAL_UINT16(1u, limit);
    run_ms(9900u);
    TEST_ASSERT_EQUAL_UINT16(100u, limit);
}

/* A step with no Sweep holds the limit where it is. */
static void test_a_step_without_a_sweep_holds_the_limit(void)
{
    TEST_ASSERT_EQUAL_UINT16(100u, first());
    cells(4100u, 4000u);
    in.swept = false;
    run_ms(1000u);
    TEST_ASSERT_EQUAL_UINT16(100u, limit);
}

static void test_the_limit_is_zero_while_bms_ok_is_low(void)
{
    in.bms_ok = false;
    TEST_ASSERT_EQUAL_UINT16(0u, first());
    in.bms_ok = true;
    step();
    TEST_ASSERT_EQUAL_UINT16(100u, limit); /* followed the target meanwhile */
}

/* ---- Start ---- */

static void test_the_charge_starts_once_the_conditions_have_held_for_3_s(void)
{
    hear();
    run_ms(BMS_CHARGE_START_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));
    step(); /* the conditions held 3 s on this step */
    TEST_ASSERT_EQUAL(BMS_CHARGE_CHARGING, bms_charge_state(&c));

    const bms_charge_event_t *e = event(EVENT_CHARGE);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_WAITING, e->data[0]);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_CHARGING, e->data[1]);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_CAUSE_NONE, e->data[2]);
    TEST_ASSERT_EQUAL_UINT16(3900u, u16_at(e->data, 4));
    TEST_ASSERT_EQUAL_UINT16(10u, u16_at(e->data, 6)); /* cell 9 counted from 1 */
    TEST_ASSERT_EQUAL_UINT16(3880u, u16_at(e->data, 8));
    TEST_ASSERT_EQUAL_UINT16(101u, u16_at(e->data, 10));
    TEST_ASSERT_EQUAL_UINT16(100u, u16_at(e->data, 12));
}

static void start_charge(void)
{
    hear();
    run_ms(BMS_CHARGE_START_MS + STEP_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_CHARGING, bms_charge_state(&c));
}

/* Each condition, broken for one step, starts the 3 s again. */
static void test_any_start_condition_broken_starts_the_3_s_again(void)
{
    for (size_t i = 0; i < 4u; i++) {
        setUp();
        hear();
        run_ms(2000u);
        switch (i) {
        case 0: in.bms_ok = false; break;
        case 1: in.sdc_closed = false; break;
        case 2: in.fault_active = true; break;
        default: silence(); break;
        }
        step();
        in.bms_ok = true;
        in.sdc_closed = true;
        in.fault_active = false;
        hear();
        run_ms(BMS_CHARGE_START_MS);
        TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));
        step();
        TEST_ASSERT_EQUAL(BMS_CHARGE_CHARGING, bms_charge_state(&c));
    }
}

static void test_a_charge_limit_of_zero_never_starts_a_charge(void)
{
    cells(4100u, 4000u);
    hear();
    run_ms(30000u);
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));
}

/* ---- The request ---- */

static void test_while_charging_the_request_asks_for_the_charge_limit_at_128_x_the_target(void)
{
    start_charge();
    const struct can_control_chg_request_t r = request();
    TEST_ASSERT_EQUAL_UINT16(5248u, r.max_charge_voltage); /* 128 x 4.100 V = 524.8 V */
    TEST_ASSERT_EQUAL_UINT16(100u, r.max_charge_current);
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_START_CHOICE, r.control);
}

static void test_otherwise_the_request_is_stop_with_0_v_and_0_a(void)
{
    hear();
    step();
    struct can_control_chg_request_t r = request();
    TEST_ASSERT_EQUAL_UINT16(0u, r.max_charge_voltage);
    TEST_ASSERT_EQUAL_UINT16(0u, r.max_charge_current);
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE, r.control);
}

/* ---- Timeout and return ---- */

static void test_a_silent_charger_stops_the_charge_and_its_return_restarts_it(void)
{
    start_charge();
    silence();
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));
    const bms_charge_event_t *e = event(EVENT_CHARGE);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_CHARGING, e->data[0]);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_WAITING, e->data[1]);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_CAUSE_SILENT, e->data[2]);
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE, request().control);

    run_ms(20000u);
    TEST_ASSERT_TRUE(heard.charge_mode);
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));

    hear();
    run_ms(BMS_CHARGE_START_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_CHARGING, bms_charge_state(&c));
}

static void test_a_bms_fault_or_the_sense_opening_stops_the_charge_with_its_cause(void)
{
    start_charge();
    in.fault_active = true;
    in.bms_ok = false;
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_CAUSE_BMS_NOT_OK, event(EVENT_CHARGE)->data[2]);

    setUp();
    start_charge();
    in.sdc_closed = false;
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_WAITING, bms_charge_state(&c));
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_CAUSE_SENSE_OPEN, event(EVENT_CHARGE)->data[2]);
}

/* ---- End ---- */

/* The highest cell at the target: the voltage taper gives 0 and the limit
   falls through 0.5 A within 500 ms. */
static void reach_the_target(void)
{
    start_charge();
    cells(4100u, 4060u);
    run_ms(480u);
    TEST_ASSERT_LESS_THAN_UINT16(BMS_CHARGE_END_LIMIT_DA, limit);
}

static void test_the_charge_completes_after_10_s_under_half_an_amp_at_the_target(void)
{
    reach_the_target();
    run_ms(BMS_CHARGE_END_MS - STEP_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_CHARGING, bms_charge_state(&c));
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_COMPLETE, bms_charge_state(&c));

    const bms_charge_event_t *e = event(EVENT_CHARGE);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_COMPLETE, e->data[1]);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_CAUSE_TARGET, e->data[2]);
    TEST_ASSERT_EQUAL_UINT16(4100u, u16_at(e->data, 4));
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE, request().control);
}

/* Under half an amp but the highest cell a little short of the target:
   the taper is still feeding it, so the charge goes on. */
static void test_a_low_limit_with_the_highest_cell_short_of_the_target_is_no_end(void)
{
    start_charge();
    cells(4098u, 4060u); /* 4% of 10 A: 0.4 A */
    run_ms(60000u);
    TEST_ASSERT_EQUAL_UINT16(4u, limit);
    TEST_ASSERT_EQUAL(BMS_CHARGE_CHARGING, bms_charge_state(&c));
}

/* The highest cell leaving the target restarts the 10 s. */
static void test_the_end_condition_broken_starts_the_10_s_again(void)
{
    reach_the_target();
    run_ms(5000u);
    cells(4099u, 4060u);
    step();
    cells(4100u, 4060u);
    run_ms(BMS_CHARGE_END_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_CHARGING, bms_charge_state(&c));
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_COMPLETE, bms_charge_state(&c));
}

static void complete_charge(void)
{
    reach_the_target();
    run_ms(BMS_CHARGE_END_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_COMPLETE, bms_charge_state(&c));
}

/* Charge complete lasts until a power cycle, whatever comes back. */
static void test_after_charge_complete_nothing_restarts_the_charge(void)
{
    complete_charge();
    cells(3950u, 3940u); /* relaxed well under the target */
    silence();
    run_ms(10000u);
    hear();
    run_ms(60000u);
    TEST_ASSERT_EQUAL(BMS_CHARGE_COMPLETE, bms_charge_state(&c));
    TEST_ASSERT_EQUAL_UINT16(0u, limit);
    TEST_ASSERT_EQUAL_UINT8(CAN_CONTROL_CHG_REQUEST_CONTROL_STOP_CHOICE, request().control);
}

static void test_the_rested_cells_are_written_60_s_after_the_stop_and_once(void)
{
    complete_charge();
    cells(4085u, 4050u);
    run_ms(BMS_CHARGE_RESTED_MS - STEP_MS);
    TEST_ASSERT_NULL(event(EVENT_CHARGE_RESTED));
    step();
    const bms_charge_event_t *e = event(EVENT_CHARGE_RESTED);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT16(4085u, u16_at(e->data, 0));
    TEST_ASSERT_EQUAL_UINT16(10u, u16_at(e->data, 2));
    TEST_ASSERT_EQUAL_UINT16(4050u, u16_at(e->data, 4));
    TEST_ASSERT_EQUAL_UINT16(101u, u16_at(e->data, 6));
    TEST_ASSERT_EQUAL_UINT32(BMS_CHARGE_RESTED_MS, u32_at(e->data, 8));

    for (uint32_t t = 0; t < 120000u; t += STEP_MS) {
        step();
        TEST_ASSERT_NULL(event(EVENT_CHARGE_RESTED));
    }
}

/* ---- Charger disobeys ---- */

static void test_a_charger_over_its_request_by_more_than_1_a_for_5_s_disobeys(void)
{
    start_charge();
    (void)request(); /* asks for 10.0 A */
    reportedDa = 111u;
    run_requesting(BMS_CHARGE_DISOBEY_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_OVER, heard.obey);
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_DISOBEYS, heard.obey);
    TEST_ASSERT_EQUAL_UINT16(111u, heard.reported_da);
}

static void test_exactly_1_a_over_its_request_is_obeying(void)
{
    start_charge();
    (void)request();
    reportedDa = 110u;
    run_requesting(60000u);
    TEST_ASSERT_EQUAL(BMS_CHARGE_OBEYS, heard.obey);
}

static void test_a_charger_over_1_a_for_5_s_after_a_stop_disobeys(void)
{
    start_charge();
    (void)request();
    silence();
    step();
    (void)request(); /* the stop */
    hear();
    reportedDa = 11u;
    run_ms(BMS_CHARGE_DISOBEY_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_OVER, heard.obey);
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_DISOBEYS, heard.obey);
}

/* Before any request, the charger has been asked for nothing. */
static void test_a_charger_delivering_before_any_request_disobeys(void)
{
    hear();
    reportedDa = 20u;
    run_ms(BMS_CHARGE_DISOBEY_MS + STEP_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_DISOBEYS, heard.obey);
}

/* No request for 5 s: the charger should have stopped by itself. */
static void test_a_request_older_than_the_chargers_own_timeout_asks_for_nothing(void)
{
    start_charge();
    (void)request(); /* 10.0 A, and then no more requests */
    reportedDa = 100u;
    run_ms(BMS_CHARGE_CHARGER_STOPS_MS + BMS_CHARGE_DISOBEY_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_OVER, heard.obey);
    step();
    TEST_ASSERT_EQUAL(BMS_CHARGE_DISOBEYS, heard.obey);
}

static void test_one_frame_within_the_margin_starts_the_5_s_again(void)
{
    hear();
    reportedDa = 20u;
    run_ms(4000u);
    reportedDa = 10u;
    step();
    reportedDa = 20u;
    run_ms(BMS_CHARGE_DISOBEY_MS);
    TEST_ASSERT_EQUAL(BMS_CHARGE_OVER, heard.obey);
}

/* A silent charger reads the Substitute, 0 A. */
static void test_a_silent_charger_never_disobeys(void)
{
    hear();
    reportedDa = 200u;
    step();
    silence();
    run_ms(60000u);
    TEST_ASSERT_EQUAL(BMS_CHARGE_OBEYS, heard.obey);
}

/* ---- Charger flags ---- */

static void test_each_change_of_the_chargers_flags_is_an_event_and_no_fault(void)
{
    hear();
    step();
    TEST_ASSERT_NULL(event(EVENT_CHARGER_FLAGS));

    flags = 0x02u; /* over-temperature */
    reportedDa = 5u;
    step();
    const bms_charge_event_t *e = event(EVENT_CHARGER_FLAGS);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(0x00u, e->data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02u, e->data[1]);
    TEST_ASSERT_EQUAL_UINT8(BMS_CHARGE_WAITING, e->data[2]);
    TEST_ASSERT_EQUAL_UINT16(4800u, u16_at(e->data, 4));
    TEST_ASSERT_EQUAL_UINT16(5u, u16_at(e->data, 6));
    TEST_ASSERT_EQUAL(BMS_CHARGE_OBEYS, heard.obey);

    run_ms(1000u);
    TEST_ASSERT_NULL(event(EVENT_CHARGER_FLAGS)); /* no change */

    /* Silence reads no flags, but is no change of the charger's. */
    silence();
    run_ms(10000u);
    TEST_ASSERT_NULL(event(EVENT_CHARGER_FLAGS));
    hear();
    step();
    TEST_ASSERT_NULL(event(EVENT_CHARGER_FLAGS));

    flags = 0x1Bu;
    step();
    e = event(EVENT_CHARGER_FLAGS);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(0x02u, e->data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x1Bu, e->data[1]);
}

/* A flag set while charging does not stop the charge. */
static void test_a_charger_flag_does_not_stop_the_charge(void)
{
    start_charge();
    flags = 0x01u;
    run_ms(10000u);
    TEST_ASSERT_EQUAL(BMS_CHARGE_CHARGING, bms_charge_state(&c));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_with_no_charger_heard_it_is_not_charge_mode_and_the_limit_is_zero);
    RUN_TEST(test_charge_mode_begins_on_the_first_good_frame_and_lasts_through_silence);
    RUN_TEST(test_a_cool_pack_well_under_the_target_gets_the_full_charge_limit);
    RUN_TEST(test_the_voltage_taper_runs_from_50_mv_under_the_target_to_zero_at_it);
    RUN_TEST(test_the_hot_taper_runs_from_50_c_to_zero_at_55_c);
    RUN_TEST(test_the_cold_taper_runs_from_10_c_to_zero_at_5_c);
    RUN_TEST(test_the_lowest_taper_wins);
    RUN_TEST(test_the_limit_moves_at_the_discharge_limits_bounded_rates);
    RUN_TEST(test_a_step_without_a_sweep_holds_the_limit);
    RUN_TEST(test_the_limit_is_zero_while_bms_ok_is_low);
    RUN_TEST(test_the_charge_starts_once_the_conditions_have_held_for_3_s);
    RUN_TEST(test_any_start_condition_broken_starts_the_3_s_again);
    RUN_TEST(test_a_charge_limit_of_zero_never_starts_a_charge);
    RUN_TEST(test_while_charging_the_request_asks_for_the_charge_limit_at_128_x_the_target);
    RUN_TEST(test_otherwise_the_request_is_stop_with_0_v_and_0_a);
    RUN_TEST(test_a_silent_charger_stops_the_charge_and_its_return_restarts_it);
    RUN_TEST(test_a_bms_fault_or_the_sense_opening_stops_the_charge_with_its_cause);
    RUN_TEST(test_the_charge_completes_after_10_s_under_half_an_amp_at_the_target);
    RUN_TEST(test_a_low_limit_with_the_highest_cell_short_of_the_target_is_no_end);
    RUN_TEST(test_the_end_condition_broken_starts_the_10_s_again);
    RUN_TEST(test_after_charge_complete_nothing_restarts_the_charge);
    RUN_TEST(test_the_rested_cells_are_written_60_s_after_the_stop_and_once);
    RUN_TEST(test_a_charger_over_its_request_by_more_than_1_a_for_5_s_disobeys);
    RUN_TEST(test_exactly_1_a_over_its_request_is_obeying);
    RUN_TEST(test_a_charger_over_1_a_for_5_s_after_a_stop_disobeys);
    RUN_TEST(test_a_charger_delivering_before_any_request_disobeys);
    RUN_TEST(test_a_request_older_than_the_chargers_own_timeout_asks_for_nothing);
    RUN_TEST(test_one_frame_within_the_margin_starts_the_5_s_again);
    RUN_TEST(test_a_silent_charger_never_disobeys);
    RUN_TEST(test_each_change_of_the_chargers_flags_is_an_event_and_no_fault);
    RUN_TEST(test_a_charger_flag_does_not_stop_the_charge);
    return UNITY_END();
}
