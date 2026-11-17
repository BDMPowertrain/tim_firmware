/*
 * Host tests for the VCU's Control step as one function
 * (App/vcu/logic/Inc/vcu_control.h), issue 56.
 *
 * Seam 1 of spec #36: the test hands the step its inputs (ADC counts,
 * digital inputs, mailboxes fed with frame bytes and arrival times, the
 * Parameters, the time) and judges only what comes out: the state, the pin
 * levels, the frames as bytes, the Events.
 *
 * The fixture plays the bench script's part: it packs BMS_Status,
 * BMS_Limits and BMS_Power (with E2E Profile 11) and the inverter frames
 * every 10 ms step, and runs them through the real mailbox logic, so a
 * mailbox is VALID after three good frames exactly as on the car.
 *
 * Spec (issue 13, "States"; spec #36, "VCU image"):
 * - STARTUP to GLV_ON once tasks exist, a full set of pedal samples is in,
 *   CAN1 runs and parameters are loaded or defaulted.
 * - GLV_ON to TS_ACTIVE: Shutdown Circuit input closed, bus voltage at least
 *   90% of pack voltage, BMS_Status reporting no fault, the BMS and inverter
 *   mailboxes VALID. TS_ACTIVE to GLV_ON: bus voltage below 60 V.
 * - Start procedure: Brake engaged, higher Pedal travel below 5%, no fault
 *   bit, no inverter fault, Shutdown Circuit closed, bus voltage at least
 *   90%, then a rising edge on the start button while the brake is held. A
 *   button already held does not count (EV.10.6).
 * - Ready to Drive sound 2.0 s from entry (EV.10.7), stopped at once if
 *   Ready to Drive is lost.
 *
 * The torque path (issue 57; spec #36, reaction table; ADR 0006):
 * - Pedal map: 0 N.m below 5%, linear to the max_torque Parameter at 100%,
 *   from the lower Pedal travel; never negative.
 * - APPS implausible (more than 10 points apart, or either not OK) for
 *   80 ms, either brake sensor failed for 80 ms, or Brake engaged with the
 *   higher travel above 25%, is a Torque cut: Enable=1, zero torque, until
 *   the cause is gone and the higher travel is below 5%.
 * - Three Overruns within a second are a Torque cut ("timing fault").
 */
#include <string.h>

#include "adc_window.h"
#include "can_control.h"
#include "control_rx.h"
#include "e2e_p11.h"
#include "event_codes.h"
#include "mailbox.h"
#include "unity.h"
#include "vcu_control.h"
#include "vcu_params.h"

#define STEP_MS 10u

/* Counts on the sensors (vcu_sensors.h): APPS calibrated over 1000-3000,
   APPS1 rising and APPS2 falling; brakes at a 372-count zero; 5 V
   supplies. */
#define APPS_MIN     1000u
#define APPS_MAX     3000u
#define BRAKE_ZERO   372u
#define BRAKE_THRESH 200u /* the brake_engaged Parameter, counts above zero */
#define SUPPLY_OK    3722u

#define PACK_DV 4000u /* 400.0 V */

#define BUILD_ID     0x1234ABCDu /* the running image's */
#define SAVE_COUNTER 7u          /* the loaded Parameters' save counter */

static vcu_control_t vc;
static vcu_control_config_t config;
static vcu_control_in_t in;
static vcu_control_out_t out;

/* The mailboxes, as control_bus.c keeps them, and what the bench sends. */
static mailbox_t mb[VCU_RX_COUNT];
static mailbox_rx_t rx[VCU_RX_COUNT];
static bool playing[VCU_RX_COUNT];
static e2e_p11_tx_t bmsTx[3];
static uint8_t lastStatus[CAN_CONTROL_VCU_STATUS_LENGTH]; /* the last VCU_Status sent */
static bool statusSent;
static unsigned passes; /* EVENT_PEDAL_CHECK Events written since setUp */

static struct can_control_bms_status_t bmsStatus;
static struct can_control_bms_limits_t bmsLimits;
static struct can_control_bms_power_t bmsPower;
static struct can_control_inv_voltage_t invVoltage;
static struct can_control_inv_fault_codes_t invFaults;

/* ------------------------------------------------------------------ */
/* The bench                                                          */
/* ------------------------------------------------------------------ */

static void set_pedal(uint16_t travel_tenths)
{
    const uint32_t span = APPS_MAX - APPS_MIN;
    in.raw.apps[0] = (uint16_t)(APPS_MIN + (span * travel_tenths) / 1000u);
    in.raw.apps[1] = (uint16_t)(APPS_MAX - (span * travel_tenths) / 1000u);
}

/* Each APPS at its own travel, in tenths of a percent. */
static void set_pedals(uint16_t apps1_tenths, uint16_t apps2_tenths)
{
    const uint32_t span = APPS_MAX - APPS_MIN;
    in.raw.apps[0] = (uint16_t)(APPS_MIN + (span * apps1_tenths) / 1000u);
    in.raw.apps[1] = (uint16_t)(APPS_MAX - (span * apps2_tenths) / 1000u);
}

/* Both brake sensors at this many counts above their zero. */
static void set_brake(uint16_t counts)
{
    in.raw.brake[0] = (uint16_t)(BRAKE_ZERO + counts);
    in.raw.brake[1] = (uint16_t)(BRAKE_ZERO + counts);
}

static void brake_on(void) { set_brake(BRAKE_THRESH + 100u); }
static void brake_off(void) { set_brake(0u); }

static void set_bus_dv(int16_t dv) { invVoltage.dc_bus_voltage = dv; }

static void play_all(bool on)
{
    for (size_t i = 0; i < VCU_RX_COUNT; i++) {
        playing[i] = on;
    }
}

static void pack_frame(size_t row, uint8_t *frame)
{
    memset(frame, 0, MAILBOX_FRAME_MAX);
    switch (row) {
    case VCU_RX_BMS_STATUS:
        (void)can_control_bms_status_pack(frame, &bmsStatus, 8u);
        e2e_p11_protect(&bmsTx[0], 0x0080u, frame, 8u);
        break;
    case VCU_RX_BMS_LIMITS:
        (void)can_control_bms_limits_pack(frame, &bmsLimits, 8u);
        e2e_p11_protect(&bmsTx[1], 0x0081u, frame, 8u);
        break;
    case VCU_RX_BMS_POWER:
        (void)can_control_bms_power_pack(frame, &bmsPower, 8u);
        e2e_p11_protect(&bmsTx[2], 0x0082u, frame, 8u);
        break;
    case VCU_RX_INV_VOLTAGE:
        (void)can_control_inv_voltage_pack(frame, &invVoltage, 8u);
        break;
    case VCU_RX_INV_FAULT_CODES:
        (void)can_control_inv_fault_codes_pack(frame, &invFaults, 8u);
        break;
    default:
        /* Every other frame is all zeros: 0 rpm, VSM start, and so on. */
        break;
    }
}

/* One 10 ms step: the bench's frames arrive, the mailboxes update, the
   step runs. */
static void step(void)
{
    for (size_t i = 0; i < VCU_RX_COUNT; i++) {
        if (playing[i]) {
            uint8_t frame[MAILBOX_FRAME_MAX];
            pack_frame(i, frame);
            mailbox_rx_store(&rx[i], frame, 8u, in.now_ms);
        }
        (void)mailbox_update(&mb[i], &control_rx_config[vcu_rx_frames[i]], &rx[i],
                             in.now_ms);
    }
    vcu_control_step(&vc, &in, &out);
    for (size_t i = 0; i < out.event_count; i++) {
        passes += out.events[i].code == EVENT_PEDAL_CHECK ? 1u : 0u;
    }
    if (out.send_status) {
        memcpy(lastStatus, out.status, sizeof(lastStatus));
        statusSent = true;
    }
    in.now_ms += STEP_MS;
    in.step_count++;
}

static void run_ms(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += STEP_MS) {
        step();
    }
}

/* The last VCU_Status sent: it goes every second step. */
static struct can_control_vcu_status_t status_frame(void)
{
    struct can_control_vcu_status_t s;
    TEST_ASSERT_TRUE(statusSent);
    TEST_ASSERT_EQUAL_INT(0, can_control_vcu_status_unpack(&s, lastStatus, sizeof(lastStatus)));
    return s;
}

static struct can_control_inv_command_t command_frame(void)
{
    struct can_control_inv_command_t c;
    TEST_ASSERT_TRUE(out.send_command);
    TEST_ASSERT_EQUAL_INT(0, can_control_inv_command_unpack(&c, out.command, sizeof(out.command)));
    return c;
}

/* The last EVENT_VCU_STATE Event of this step. */
static const vcu_event_t *state_event(void)
{
    for (size_t i = out.event_count; i > 0u; i--) {
        if (out.events[i - 1u].code == EVENT_VCU_STATE) {
            return &out.events[i - 1u];
        }
    }
    return NULL;
}

static void assert_transition(uint8_t from, uint8_t to, vcu_cause_t cause)
{
    const vcu_event_t *ev = state_event();
    TEST_ASSERT_NOT_NULL_MESSAGE(ev, "no state Event this step");
    TEST_ASSERT_EQUAL_UINT8(from, ev->data[0]);
    TEST_ASSERT_EQUAL_UINT8(to, ev->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)cause, ev->data[2]);
    TEST_ASSERT_EQUAL_UINT8(to, out.state);
}

/* Brings the VCU to GLV_ON with no frames on the bus. */
static void to_glv_on(void)
{
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
}

/* Brings the VCU to TS_ACTIVE: every frame played, the bus charged. */
static void to_ts_active(void)
{
    to_glv_on();
    play_all(true);
    run_ms(3u * STEP_MS); /* three good frames: every mailbox VALID */
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
}

/* The Start procedure: brake held, then a press of the start button. */
static void to_ready_to_drive(void)
{
    to_ts_active();
    brake_on();
    run_ms(VCU_START_RELEASE_MS); /* the button has been released long enough */
    in.start_pressed = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
}

/* A press of the start button after it has been released long enough. */
static void press_start(void)
{
    in.start_pressed = false;
    run_ms(VCU_START_RELEASE_MS);
    in.start_pressed = true;
    step();
}

/* In TS_ACTIVE with the brake held, a press is refused for blocker. */
static void assert_start_refused(uint16_t blocker)
{
    press_start();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
    TEST_ASSERT_NULL(state_event());
    TEST_ASSERT_BITS_HIGH(blocker, out.start_blockers);
}

void setUp(void)
{
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    memset(mb, 0, sizeof(mb));
    memset(rx, 0, sizeof(rx));
    memset(bmsTx, 0, sizeof(bmsTx));
    statusSent = false;
    passes = 0u;
    play_all(false);

    config = (vcu_control_config_t){
        .cal = {
            .apps_min = { APPS_MIN, APPS_MIN },
            .apps_max = { APPS_MAX, APPS_MAX },
            .brake_zero = { BRAKE_ZERO, BRAKE_ZERO },
        },
        .brake_engaged = BRAKE_THRESH,
        .cal_revision = VCU_CALIBRATION_REVISION,
        .cal_present = true,
        .max_torque = 1000u, /* 100.0 N.m */
        .reset_ms = 0u,
        .build_id = BUILD_ID,
        .param_save_counter = SAVE_COUNTER,
    };
    vcu_control_init(&vc, &config);

    in.now_ms = 20u; /* the first step, a little after reset release */
    in.pedal_check_record = VCU_PEDAL_CHECK_FOUND; /* this image's check passed before */
    in.startup_ok = true;
    in.can_running = true;
    in.raw.supply[0] = SUPPLY_OK;
    in.raw.supply[1] = SUPPLY_OK;
    set_pedal(0u);
    brake_off();
    in.sdc_closed = true;
    in.mailboxes = mb;

    /* A healthy BMS and inverter, Tractive System up. */
    bmsStatus = (struct can_control_bms_status_t){ .state = 1u, .bms_ok = 1u };
    bmsLimits = (struct can_control_bms_limits_t){ .pack_voltage = PACK_DV };
    bmsPower = (struct can_control_bms_power_t){ .pack_voltage = PACK_DV };
    invVoltage = (struct can_control_inv_voltage_t){ .dc_bus_voltage = (int16_t)PACK_DV };
    invFaults = (struct can_control_inv_fault_codes_t){ 0 };
}

void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* STARTUP                                                            */
/* ------------------------------------------------------------------ */

static void test_startup_goes_to_glv_on_on_the_first_step_with_everything_ready(void)
{
    step();
    assert_transition(VCU_STATE_STARTUP, VCU_STATE_GLV_ON, VCU_CAUSE_STARTUP_DONE);
    TEST_ASSERT_EQUAL_UINT8(0u, status_frame().startup_blocked);
}

static void test_startup_stays_while_a_task_or_queue_is_missing(void)
{
    in.startup_ok = false;
    run_ms(1000u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_STARTUP, out.state);
    TEST_ASSERT_NULL(state_event());
    TEST_ASSERT_TRUE(out.startup_blocked);
    TEST_ASSERT_TRUE(out.fault_active);
}


static void test_startup_waits_for_can1_and_a_full_set_of_samples(void)
{
    in.can_running = false;
    in.raw.brake[1] = ADC_WINDOW_EMPTY;
    run_ms(100u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_STARTUP, out.state);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().startup_blocked);

    in.can_running = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_STARTUP, out.state);

    brake_off();
    step();
    assert_transition(VCU_STATE_STARTUP, VCU_STATE_GLV_ON, VCU_CAUSE_STARTUP_DONE);
}

/* Spec #36: defaults never allow driving, but the VCU reaches GLV_ON. */
static void test_startup_ends_with_parameters_defaulted(void)
{
    config.params_defaulted = true;
    vcu_control_init(&vc, &config);
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
}

static void test_startup_sends_no_inverter_command(void)
{
    in.startup_ok = false;
    run_ms(100u);
    TEST_ASSERT_FALSE(out.send_command);
}

/* ------------------------------------------------------------------ */
/* GLV_ON and TS_ACTIVE                                               */
/* ------------------------------------------------------------------ */

static void test_glv_on_goes_to_ts_active_once_every_mailbox_is_valid(void)
{
    to_glv_on();
    play_all(true);
    run_ms(2u * STEP_MS); /* two good frames: not VALID yet */
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
    step();
    assert_transition(VCU_STATE_GLV_ON, VCU_STATE_TS_ACTIVE, VCU_CAUSE_TS_UP);
}

static void test_glv_on_stays_with_the_shutdown_circuit_open(void)
{
    to_glv_on();
    in.sdc_closed = false;
    play_all(true);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().shutdown_circuit_open);
}

static void test_glv_on_stays_while_the_bms_reports_a_fault(void)
{
    to_glv_on();
    bmsStatus.bms_ok = 0u;
    bmsStatus.state = 3u; /* FAULT */
    play_all(true);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().bms_fault_reported);
}

static void test_glv_on_stays_while_a_bms_fault_bit_is_active(void)
{
    to_glv_on();
    bmsStatus.over_temp_active = 1u;
    play_all(true);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
}

static void test_glv_on_stays_with_any_bms_or_inverter_mailbox_silent(void)
{
    static const vcu_rx_t needed[] = {
        VCU_RX_BMS_STATUS, VCU_RX_BMS_LIMITS, VCU_RX_BMS_POWER,
        VCU_RX_INV_MOTOR_POSITION, VCU_RX_INV_VOLTAGE,
        VCU_RX_INV_INTERNAL_STATES, VCU_RX_INV_FAULT_CODES,
    };

    for (size_t n = 0; n < sizeof(needed) / sizeof(needed[0]); n++) {
        setUp();
        to_glv_on();
        play_all(true);
        playing[needed[n]] = false;
        run_ms(500u);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(VCU_STATE_GLV_ON, out.state, "a needed frame was silent");
    }
}

static void test_glv_on_does_not_need_the_logged_only_frames(void)
{
    to_glv_on();
    play_all(true);
    playing[VCU_RX_INV_TORQUE_CAPABILITY] = false;
    playing[VCU_RX_EM_MEASUREMENT] = false;
    playing[VCU_RX_EM_STATUS] = false;
    playing[VCU_RX_IMD_INFO_GENERAL] = false;
    run_ms(3u * STEP_MS);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().energy_meter_timeout);
}

static void test_glv_on_needs_the_bus_at_90_percent_of_the_pack(void)
{
    to_glv_on();
    play_all(true);
    set_bus_dv(3599); /* 359.9 V of 400.0 V */
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);

    set_bus_dv(3600); /* exactly 90% */
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
}

/* A BMS with no pack voltage must not make 0 V look like 90% of it. */
static void test_glv_on_needs_the_bus_at_60_v_whatever_the_pack_says(void)
{
    to_glv_on();
    bmsLimits.pack_voltage = 0u;
    set_bus_dv(0);
    play_all(true);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
}

/* EV.5.4.5: below 60 V the Tractive System is discharged. */
static void test_ts_active_returns_to_glv_on_below_60_v(void)
{
    to_ts_active();
    set_bus_dv(600);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);

    set_bus_dv(599);
    step();
    assert_transition(VCU_STATE_TS_ACTIVE, VCU_STATE_GLV_ON, VCU_CAUSE_BUS_DISCHARGED);
    TEST_ASSERT_EQUAL_INT16(599, (int16_t)(out.events[0].data[4] | (out.events[0].data[5] << 8)));
}

/* TS_ACTIVE covers a car that is discharging: the Shutdown Circuit opening
   does not end it while the bus holds voltage. */
static void test_ts_active_lasts_while_the_bus_discharges(void)
{
    to_ts_active();
    in.sdc_closed = false;
    set_bus_dv(1000);
    run_ms(1000u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
}

/* The inverter going silent gives its Substitute, 0 V, which is no news
   of a discharged bus: TS_ACTIVE stays, and the Start procedure is
   refused for the mailbox. */
static void test_a_silent_inverter_does_not_end_ts_active(void)
{
    to_ts_active();
    playing[VCU_RX_INV_VOLTAGE] = false;
    run_ms(1000u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
    TEST_ASSERT_NULL(state_event());
    TEST_ASSERT_BITS_HIGH(VCU_START_MAILBOX, out.start_blockers);

    /* Back, and still discharged: GLV_ON once it is VALID again. */
    set_bus_dv(0);
    playing[VCU_RX_INV_VOLTAGE] = true;
    run_ms(3u * STEP_MS);
    assert_transition(VCU_STATE_TS_ACTIVE, VCU_STATE_GLV_ON, VCU_CAUSE_BUS_DISCHARGED);
}

/* ------------------------------------------------------------------ */
/* Brake engaged and the brake light                                  */
/* ------------------------------------------------------------------ */

static void test_brake_engaged_is_above_the_threshold_not_at_it(void)
{
    to_glv_on();
    set_brake(BRAKE_THRESH);
    step();
    TEST_ASSERT_FALSE(out.brake_engaged);
    TEST_ASSERT_FALSE(out.brake_light);

    set_brake(BRAKE_THRESH + 1u);
    step();
    TEST_ASSERT_TRUE(out.brake_engaged);
    TEST_ASSERT_TRUE(out.brake_light);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().brake_engaged);
}

static void test_brake_engaged_releases_only_below_its_hysteresis(void)
{
    to_glv_on();
    set_brake(BRAKE_THRESH + 1u);
    step();
    TEST_ASSERT_TRUE(out.brake_engaged);

    set_brake(BRAKE_THRESH - VCU_BRAKE_HYSTERESIS + 1u);
    step();
    TEST_ASSERT_TRUE(out.brake_engaged);

    set_brake(BRAKE_THRESH - VCU_BRAKE_HYSTERESIS);
    step();
    TEST_ASSERT_FALSE(out.brake_engaged);
    TEST_ASSERT_FALSE(out.brake_light);
}

static void test_either_brake_sensor_alone_engages_the_brake(void)
{
    to_glv_on();
    in.raw.brake[1] = (uint16_t)(BRAKE_ZERO + BRAKE_THRESH + 50u);
    step();
    TEST_ASSERT_TRUE(out.brake_engaged);

    brake_off();
    run_ms(20u);
    in.raw.brake[0] = (uint16_t)(BRAKE_ZERO + BRAKE_THRESH + 50u);
    step();
    TEST_ASSERT_TRUE(out.brake_engaged);
}

/* A sensor shorted to its supply reads high but is not a pressed brake. */
static void test_a_failed_brake_sensor_does_not_engage_the_brake(void)
{
    to_glv_on();
    in.raw.brake[1] = 3800u; /* above the 4.75 V window */
    step();
    TEST_ASSERT_EQUAL(VCU_SENSOR_OUT_OF_RANGE, out.sensors.brake[1].status);
    TEST_ASSERT_FALSE(out.brake_engaged);
}

/* Issue 13: the brake light is driven in every state, STARTUP included
   once the ADC runs. */
static void test_the_brake_light_follows_the_brake_in_startup(void)
{
    in.startup_ok = false;
    brake_on();
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_STARTUP, out.state);
    TEST_ASSERT_TRUE(out.brake_light);
}

static void test_the_brake_light_follows_the_brake_in_ts_active(void)
{
    to_ts_active();
    brake_on();
    step();
    TEST_ASSERT_TRUE(out.brake_light);
    brake_off();
    step();
    TEST_ASSERT_FALSE(out.brake_light);
}

/* ------------------------------------------------------------------ */
/* The Start procedure (EV.10.6)                                      */
/* ------------------------------------------------------------------ */

/* EV.10.6: Ready to Drive needs the brake held and a press of the start
   button. */
static void test_the_start_procedure_enters_ready_to_drive(void)
{
    to_ts_active();
    brake_on();
    run_ms(VCU_START_RELEASE_MS);
    TEST_ASSERT_EQUAL_UINT16(0u, out.start_blockers);
    in.start_pressed = true;
    step();
    assert_transition(VCU_STATE_TS_ACTIVE, VCU_STATE_READY_TO_DRIVE, VCU_CAUSE_START_PROCEDURE);
}

/* EV.10.6: a start button already held when the brake goes down does not
   count, however long both are held. */
static void test_a_button_held_before_the_brake_does_not_start(void)
{
    to_ts_active();
    press_start(); /* no brake: refused */
    brake_on();
    run_ms(2000u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);

    /* Let go and press again with the brake still held: that counts. */
    press_start();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
}

static void test_the_brake_and_the_button_in_the_same_step_do_not_start(void)
{
    to_ts_active();
    run_ms(VCU_START_RELEASE_MS);
    brake_on();
    in.start_pressed = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
}

/* A held button that bounces as it is let go is not a new press. */
static void test_a_bounce_shorter_than_the_debounce_is_not_a_press(void)
{
    to_ts_active();
    in.start_pressed = true;
    step();
    brake_on();
    run_ms(100u);
    in.start_pressed = false;
    run_ms(VCU_START_RELEASE_MS - STEP_MS);
    in.start_pressed = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
}

/* A button held since reset (stuck, or held through a reset) never counts
   until it has been let go. */
static void test_a_button_held_since_reset_never_counts(void)
{
    in.start_pressed = true;
    brake_on();
    to_ts_active();
    run_ms(1000u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
}

static void test_start_is_refused_in_glv_on(void)
{
    to_glv_on();
    brake_on();
    press_start();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
    TEST_ASSERT_BITS_HIGH(VCU_START_NOT_TS_ACTIVE, out.start_blockers);
}

static void test_start_is_refused_without_the_brake(void)
{
    to_ts_active();
    assert_start_refused(VCU_START_BRAKE);
}

/* The 5% release uses the higher Pedal travel. */
static void test_start_is_refused_with_the_pedal_at_5_percent(void)
{
    to_ts_active();
    brake_on();
    set_pedal(50u); /* 5.0% */
    assert_start_refused(VCU_START_PEDAL);

    set_pedal(49u);
    press_start();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
}

static void test_start_is_refused_with_one_pedal_sensor_pressed(void)
{
    to_ts_active();
    brake_on();
    in.raw.apps[1] = (uint16_t)(APPS_MAX - 200u); /* APPS2 alone at 10% */
    assert_start_refused(VCU_START_PEDAL);
}

/* A broken pedal sensor reads 0% travel: that must not pass for released. */
static void test_start_is_refused_with_a_pedal_sensor_failed(void)
{
    to_ts_active();
    brake_on();
    in.raw.apps[0] = 50u; /* far below its min: open circuit */
    assert_start_refused(VCU_START_PEDAL_SENSOR);
}

static void test_start_is_refused_with_a_brake_sensor_failed(void)
{
    to_ts_active();
    brake_on();
    in.raw.brake[1] = 3800u;
    assert_start_refused(VCU_START_BRAKE_SENSOR);
}

static void test_start_is_refused_with_parameters_defaulted(void)
{
    config.params_defaulted = true;
    vcu_control_init(&vc, &config);
    to_ts_active();
    brake_on();
    assert_start_refused(VCU_START_PARAMS_DEFAULTED);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().parameters_defaulted);
}

static void test_start_is_refused_with_a_mailbox_not_valid(void)
{
    to_ts_active();
    brake_on();
    playing[VCU_RX_BMS_POWER] = false;
    run_ms(100u); /* its timeout */
    assert_start_refused(VCU_START_MAILBOX);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().bms_power_timeout);
}

static void test_start_is_refused_while_the_bms_reports_a_fault(void)
{
    to_ts_active();
    brake_on();
    bmsStatus.under_voltage_active = 1u;
    assert_start_refused(VCU_START_BMS_FAULT);
}

static void test_start_is_refused_while_the_inverter_reports_a_fault(void)
{
    to_ts_active();
    brake_on();
    invFaults.run_faults = 0x00000100u;
    assert_start_refused(VCU_START_INVERTER_FAULT);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().inverter_fault_reported);
}

static void test_start_is_refused_with_the_shutdown_circuit_open(void)
{
    to_ts_active();
    brake_on();
    in.sdc_closed = false;
    assert_start_refused(VCU_START_SDC_OPEN);
}

static void test_start_is_refused_with_the_bus_off_latch_set(void)
{
    to_ts_active();
    brake_on();
    in.bus_off_latched = true;
    assert_start_refused(VCU_START_BUS_OFF);
}

static void test_start_is_refused_with_the_bus_below_90_percent(void)
{
    to_ts_active();
    brake_on();
    set_bus_dv(3599);
    assert_start_refused(VCU_START_BUS_VOLTAGE);
}

static void test_start_is_refused_on_a_timing_fault(void)
{
    to_ts_active();
    brake_on();
    in.timing_fault = true;
    assert_start_refused(VCU_START_TIMING_FAULT);
}

/* ------------------------------------------------------------------ */
/* Leaving Ready to Drive (ADR 0006)                                  */
/* ------------------------------------------------------------------ */

static uint16_t event_causes(void)
{
    const vcu_event_t *ev = state_event();
    TEST_ASSERT_NOT_NULL(ev);
    return (uint16_t)(ev->data[8] | (ev->data[9] << 8));
}

static uint16_t cause_bit(vcu_cause_t cause)
{
    return (uint16_t)(1u << (cause - VCU_CAUSE_SDC_OPEN));
}

static void test_ready_to_drive_is_left_when_the_shutdown_circuit_opens(void)
{
    to_ready_to_drive();
    in.sdc_closed = false;
    step();
    assert_transition(VCU_STATE_READY_TO_DRIVE, VCU_STATE_TS_ACTIVE, VCU_CAUSE_SDC_OPEN);
    TEST_ASSERT_EQUAL_HEX16(cause_bit(VCU_CAUSE_SDC_OPEN), event_causes());
}

static void test_ready_to_drive_is_left_when_the_bms_reports_a_fault(void)
{
    to_ready_to_drive();
    bmsStatus.over_voltage_active = 1u;
    step();
    assert_transition(VCU_STATE_READY_TO_DRIVE, VCU_STATE_TS_ACTIVE, VCU_CAUSE_BMS_FAULT);
}

static void test_ready_to_drive_is_left_when_the_inverter_reports_a_fault(void)
{
    to_ready_to_drive();
    invFaults.post_faults = 0x1u;
    step();
    assert_transition(VCU_STATE_READY_TO_DRIVE, VCU_STATE_TS_ACTIVE, VCU_CAUSE_INVERTER_FAULT);
}

static void test_ready_to_drive_is_left_when_the_bus_off_latch_sets(void)
{
    to_ready_to_drive();
    in.bus_off_latched = true;
    step();
    assert_transition(VCU_STATE_READY_TO_DRIVE, VCU_STATE_TS_ACTIVE, VCU_CAUSE_BUS_OFF_LATCHED);
}

static void test_ready_to_drive_is_left_below_80_percent_of_the_pack(void)
{
    to_ready_to_drive();
    set_bus_dv(3200); /* exactly 80% */
    run_ms(500u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);

    set_bus_dv(3199);
    step();
    assert_transition(VCU_STATE_READY_TO_DRIVE, VCU_STATE_TS_ACTIVE, VCU_CAUSE_BUS_VOLTAGE_LOW);
}

static void test_leaving_ready_to_drive_records_every_cause_that_applied(void)
{
    to_ready_to_drive();
    in.sdc_closed = false;
    in.bus_off_latched = true;
    set_bus_dv(1000);
    step();
    assert_transition(VCU_STATE_READY_TO_DRIVE, VCU_STATE_TS_ACTIVE, VCU_CAUSE_SDC_OPEN);
    TEST_ASSERT_EQUAL_HEX16(cause_bit(VCU_CAUSE_SDC_OPEN) | cause_bit(VCU_CAUSE_BUS_OFF_LATCHED) |
                                cause_bit(VCU_CAUSE_BUS_VOLTAGE_LOW),
                            event_causes());
}

/* ADR 0006: a mailbox timeout is a Torque cut, never Leaving Ready to
   Drive, though BMS_Status's Substitute says "BMS faulted". */
static void test_a_silent_bms_does_not_leave_ready_to_drive(void)
{
    to_ready_to_drive();
    playing[VCU_RX_BMS_STATUS] = false;
    playing[VCU_RX_BMS_LIMITS] = false;
    run_ms(1000u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().bms_status_timeout);
    TEST_ASSERT_EQUAL_UINT8(0u, status_frame().bms_fault_reported);
}

/* ...and the inverter's voltage Substitute, 0 V, is not a lost bus. */
static void test_a_silent_inverter_does_not_leave_ready_to_drive(void)
{
    to_ready_to_drive();
    playing[VCU_RX_INV_VOLTAGE] = false;
    playing[VCU_RX_INV_FAULT_CODES] = false;
    run_ms(1000u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().inverter_timeout);
}

/* Back in TS_ACTIVE, Ready to Drive needs the Start procedure again. */
static void test_after_leaving_the_start_procedure_is_needed_again(void)
{
    to_ready_to_drive();
    in.sdc_closed = false;
    step();
    in.sdc_closed = true;
    run_ms(1000u); /* button still held, brake still on */
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);

    press_start();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
}

/* ------------------------------------------------------------------ */
/* The Ready to Drive sound (EV.10.7)                                 */
/* ------------------------------------------------------------------ */

/* EV.10.7: the sound lasts 1-3 s from entry; ours 2.0 s. */
static void test_the_sound_runs_2_s_from_entry(void)
{
    to_ready_to_drive();
    TEST_ASSERT_TRUE(out.sound);
    run_ms(VCU_SOUND_MS - STEP_MS); /* the last step is 1990 ms from entry */
    TEST_ASSERT_TRUE(out.sound);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().sound_on);
    step();
    TEST_ASSERT_FALSE(out.sound);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
}

static void test_the_sound_stops_at_once_when_ready_to_drive_is_left(void)
{
    to_ready_to_drive();
    run_ms(500u);
    in.sdc_closed = false;
    step();
    TEST_ASSERT_FALSE(out.sound);
}

static void test_there_is_no_sound_outside_ready_to_drive(void)
{
    to_ts_active();
    run_ms(100u);
    TEST_ASSERT_FALSE(out.sound);
}

/* EV.10.7 across the 32-bit millisecond wrap. */
static void test_the_sound_times_2_s_across_the_clock_wrap(void)
{
    in.now_ms = 0xFFFFFF00u;
    to_ready_to_drive();
    TEST_ASSERT_TRUE(out.sound);
    run_ms(VCU_SOUND_MS - STEP_MS);
    TEST_ASSERT_TRUE(out.sound);
    step();
    TEST_ASSERT_FALSE(out.sound);
}

/* EV.10.6 across the wrap: the debounce measures by subtraction. */
static void test_the_start_procedure_works_across_the_clock_wrap(void)
{
    in.now_ms = 0xFFFFFFE0u;
    to_ts_active();
    brake_on();
    run_ms(VCU_START_RELEASE_MS); /* the release spans the wrap */
    in.start_pressed = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
}

/* ------------------------------------------------------------------ */
/* The inverter command (0x0C0)                                       */
/* ------------------------------------------------------------------ */

static void test_the_command_goes_every_step_after_startup_with_enable_0(void)
{
    to_glv_on();
    for (int i = 0; i < 5; i++) {
        const struct can_control_inv_command_t c = command_frame();
        TEST_ASSERT_EQUAL_UINT8(0u, c.inverter_enable);
        TEST_ASSERT_EQUAL_UINT8(1u, c.direction_command); /* forward */
        TEST_ASSERT_EQUAL_INT16(0, c.torque_command);
        TEST_ASSERT_EQUAL_INT16(0, c.speed_command);
        TEST_ASSERT_EQUAL_UINT8(0u, c.inverter_discharge);
        TEST_ASSERT_EQUAL_UINT8(0u, c.speed_mode_enable);
        TEST_ASSERT_EQUAL_INT16(1000, c.torque_limit); /* the max_torque Parameter */
        step();
    }
}

static void test_the_command_is_sent_in_ts_active_with_enable_0(void)
{
    to_ts_active();
    TEST_ASSERT_EQUAL_UINT8(0u, command_frame().inverter_enable);
}

/* CM manual section 8.12: the rolling counter is the last value + 1,
   modulo 16, every frame. */
static void test_the_rolling_counter_counts_every_frame_modulo_16(void)
{
    to_glv_on();
    uint8_t expected = command_frame().rolling_counter;
    for (int i = 0; i < 40; i++) {
        step();
        expected = (uint8_t)((expected + 1u) % 16u);
        TEST_ASSERT_EQUAL_UINT8(expected, command_frame().rolling_counter);
    }
}

/* Issue 13: the first frame after entry carries Enable=1 and zero torque. */
static void test_the_first_command_in_ready_to_drive_enables_with_zero_torque(void)
{
    to_ready_to_drive();
    const struct can_control_inv_command_t c = command_frame();
    TEST_ASSERT_EQUAL_UINT8(1u, c.inverter_enable);
    TEST_ASSERT_EQUAL_INT16(0, c.torque_command);
    TEST_ASSERT_EQUAL_UINT8(0u, c.inverter_discharge);
}

static void test_ready_to_drive_keeps_enable_1_and_reports_it(void)
{
    to_ready_to_drive();
    run_ms(100u);
    TEST_ASSERT_EQUAL_UINT8(1u, command_frame().inverter_enable);
    const struct can_control_vcu_status_t st = status_frame();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, st.state);
    TEST_ASSERT_EQUAL_UINT8(1u, st.enable_sent);
    TEST_ASSERT_EQUAL_INT16(0, st.torque_sent);
}

static void test_leaving_ready_to_drive_sends_enable_0_in_the_same_step(void)
{
    to_ready_to_drive();
    run_ms(100u);
    invFaults.run_faults = 0x80000000u;
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
    TEST_ASSERT_EQUAL_UINT8(0u, command_frame().inverter_enable);
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, status_frame().enable_sent);
}

/* ------------------------------------------------------------------ */
/* The torque path: pedal map and gates                              */
/* ------------------------------------------------------------------ */

static int16_t torque_sent(void)
{
    return command_frame().torque_command;
}

/* Spec #36: 0 N.m below 5%, linear to max_torque (100.0 N.m) at 100%. */
static void test_the_pedal_map_is_zero_below_5_percent_and_max_at_100(void)
{
    to_ready_to_drive();
    brake_off();
    step();

    set_pedal(49u);
    step();
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());

    set_pedal(50u);
    step();
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());

    set_pedal(525u); /* halfway from 5% to 100% */
    step();
    TEST_ASSERT_EQUAL_INT16(500, torque_sent());

    set_pedal(1000u);
    step();
    TEST_ASSERT_EQUAL_INT16(1000, torque_sent());
    TEST_ASSERT_EQUAL_INT16(1000, command_frame().torque_limit);
    TEST_ASSERT_EQUAL_INT16(1000, status_frame().torque_sent);
}

/* Spec #36: the request uses the lower Pedal travel. */
static void test_the_request_uses_the_lower_pedal_travel(void)
{
    to_ready_to_drive();
    brake_off();
    set_pedals(575u, 525u);
    step();
    TEST_ASSERT_EQUAL_INT16(500, torque_sent());
    set_pedals(525u, 575u);
    step();
    TEST_ASSERT_EQUAL_INT16(500, torque_sent());
}

/* EV.3.1.3 (user story 19): the command never exceeds the request, and is
   never negative, even with max_torque at 0. */
static void test_the_command_is_never_negative(void)
{
    config.max_torque = 0u;
    vcu_control_init(&vc, &config);
    to_ready_to_drive();
    brake_off();
    for (uint16_t t = 0u; t <= 1000u; t = (uint16_t)(t + 50u)) {
        set_pedal(t);
        step();
        TEST_ASSERT_EQUAL_INT16(0, torque_sent());
    }
}

/* Spec #36: any state but READY_TO_DRIVE forces zero torque and Enable=0. */
static void test_no_torque_outside_ready_to_drive(void)
{
    to_ts_active();
    set_pedal(800u);
    run_ms(200u);
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());
    TEST_ASSERT_EQUAL_UINT8(0u, command_frame().inverter_enable);
}

/* Issue 13: the first frame after entry carries zero torque, whatever the
   pedal; the second carries the request. */
static void test_the_entry_frame_has_zero_torque_and_the_next_the_request(void)
{
    to_ts_active();
    brake_on();
    run_ms(VCU_START_RELEASE_MS);
    in.start_pressed = true;
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());
    brake_off();
    set_pedal(525u);
    step();
    TEST_ASSERT_EQUAL_INT16(500, torque_sent());
}

/* Leaving Ready to Drive zeroes torque in the same step. */
static void test_leaving_ready_to_drive_zeroes_torque_in_the_same_step(void)
{
    to_ready_to_drive();
    brake_off();
    set_pedal(525u);
    step();
    TEST_ASSERT_EQUAL_INT16(500, torque_sent());
    in.sdc_closed = false;
    step();
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());
    TEST_ASSERT_EQUAL_UINT8(0u, command_frame().inverter_enable);
}

/* ------------------------------------------------------------------ */
/* Torque cuts (ADR 0006)                                             */
/* ------------------------------------------------------------------ */

/* One step ms after the last one, not the usual 10 ms. */
static void step_after(uint32_t ms)
{
    in.now_ms = in.now_ms - STEP_MS + ms;
    step();
}

/* The EVENT_TORQUE_CUT Event for this cause this step, or NULL. */
static const vcu_event_t *cut_event(vcu_cut_t cause)
{
    for (size_t i = 0; i < out.event_count; i++) {
        if (out.events[i].code == EVENT_TORQUE_CUT && out.events[i].data[0] == (uint8_t)cause) {
            return &out.events[i];
        }
    }
    return NULL;
}

static void assert_cut_starts(vcu_cut_t cause)
{
    const vcu_event_t *ev = cut_event(cause);
    TEST_ASSERT_NOT_NULL_MESSAGE(ev, "no torque cut Event this step");
    TEST_ASSERT_EQUAL_UINT8(1u, ev->data[1]);
    TEST_ASSERT_BITS_HIGH(1u << cause, out.cuts);
}

static void assert_cut_ends(vcu_cut_t cause)
{
    const vcu_event_t *ev = cut_event(cause);
    TEST_ASSERT_NOT_NULL_MESSAGE(ev, "no torque cut Event this step");
    TEST_ASSERT_EQUAL_UINT8(0u, ev->data[1]);
    TEST_ASSERT_BITS_LOW(1u << cause, out.cuts);
}

/* In READY_TO_DRIVE, brake off, pedal at 52.5%: 50.0 N.m going out. */
static void driving(void)
{
    to_ready_to_drive();
    brake_off();
    set_pedal(525u);
    step();
    TEST_ASSERT_EQUAL_INT16(500, torque_sent());
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
}

/* T.4.2.4, T.4.2.5: more than 10 points apart for 80 ms (the rule allows
   100 ms) cuts torque; 79 ms does not. */
static void test_apps_disagreeing_for_80_ms_cuts_torque_79_ms_does_not(void)
{
    driving();
    set_pedals(640u, 525u); /* 11.5 points apart */
    step();
    for (uint32_t t = 10u; t <= 70u; t += STEP_MS) {
        step();
    }
    step_after(9u); /* 79 ms after the disagreement was first seen */
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
    TEST_ASSERT_NOT_EQUAL(0, torque_sent());
    TEST_ASSERT_FALSE(out.test_point);

    step_after(1u); /* 80 ms */
    assert_cut_starts(VCU_CUT_APPS);
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());
    TEST_ASSERT_EQUAL_UINT8(1u, command_frame().inverter_enable);
    TEST_ASSERT_TRUE(out.test_point);
    TEST_ASSERT_TRUE(out.fault_active);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
    step();
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().apps_implausible);
}

/* T.4.2.4: exactly 10 points apart is plausible. */
static void test_apps_exactly_10_points_apart_is_plausible(void)
{
    driving();
    set_pedals(625u, 525u);
    run_ms(1000u);
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
}

/* T.4.2.10 via T.4.2.4: an APPS out of range (open wire) is implausible. */
static void test_an_apps_out_of_range_for_80_ms_cuts_torque(void)
{
    driving();
    in.raw.apps[1] = 4000u; /* far above its range */
    run_ms(80u);
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
    step();
    assert_cut_starts(VCU_CUT_APPS);
}

/* A disagreement that clears before 80 ms restarts the timer. */
static void test_an_apps_glitch_under_80_ms_restarts_the_timer(void)
{
    driving();
    set_pedals(800u, 525u);
    run_ms(70u);
    set_pedal(525u);
    step();
    set_pedals(800u, 525u);
    run_ms(80u);
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
    step();
    assert_cut_starts(VCU_CUT_APPS);
}

/* ADR 0006, T.4.2.5: the cut ends once plausible and the higher travel is
   below 5%, not before; then torque returns. */
static void test_an_apps_cut_recovers_only_below_5_percent(void)
{
    driving();
    set_pedals(800u, 525u);
    run_ms(90u);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_APPS, out.cuts);

    set_pedal(300u); /* plausible again, pedal pressed */
    run_ms(500u);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_APPS, out.cuts);
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());

    set_pedal(50u); /* 5.0%: not below */
    run_ms(100u);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_APPS, out.cuts);

    set_pedal(49u);
    step();
    assert_cut_ends(VCU_CUT_APPS);
    TEST_ASSERT_FALSE(out.test_point);

    set_pedal(525u);
    step();
    TEST_ASSERT_EQUAL_INT16(500, torque_sent());
}

/* Still implausible below 5% (one sensor stuck at 0): no recovery. */
static void test_an_apps_cut_does_not_recover_while_still_implausible(void)
{
    driving();
    set_pedals(0u, 0u);
    in.raw.apps[0] = 50u; /* APPS1 open, far below its min */
    run_ms(500u);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_APPS, out.cuts);
}

/* T.4.2.5 across the 32-bit millisecond wrap. */
static void test_the_apps_timer_works_across_the_clock_wrap(void)
{
    in.now_ms = 0xFFFFFF90u;
    driving();
    TEST_ASSERT_TRUE(in.now_ms > 0xFFFFFF00u); /* the wrap is still ahead */
    set_pedals(800u, 525u); /* first seen before the wrap, trips after it */
    run_ms(80u);
    TEST_ASSERT_TRUE(in.now_ms < 0x100u);
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
    step();
    assert_cut_starts(VCU_CUT_APPS);
}

/* T.4.3.3, T.4.3.4: a brake sensor failed for 80 ms cuts torque, 79 ms
   does not; the brake light is on while the cut lasts (spec #36). */
static void test_a_brake_sensor_failed_for_80_ms_cuts_torque(void)
{
    driving();
    in.raw.brake[0] = 3800u; /* above its 4.75 V window: shorted */
    run_ms(80u);
    step_after(9u);
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
    TEST_ASSERT_TRUE(out.brake_light); /* on from the failure, not the cut */

    step_after(1u);
    assert_cut_starts(VCU_CUT_BRAKE_SENSOR);
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());
    TEST_ASSERT_EQUAL_UINT8(1u, command_frame().inverter_enable);
    TEST_ASSERT_TRUE(out.brake_light);
    TEST_ASSERT_TRUE(out.test_point);
    step();
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().brake_sensor_failed);
}

/* ADR 0006: back in range is not enough; the pedal must be below 5%. The
   brake light stays on until the cut ends. */
static void test_a_brake_sensor_cut_recovers_only_below_5_percent(void)
{
    driving();
    in.raw.brake[1] = 10u; /* below its window: open wire */
    run_ms(100u);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_BRAKE_SENSOR, out.cuts);

    brake_off(); /* in range again */
    run_ms(200u);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_BRAKE_SENSOR, out.cuts);
    TEST_ASSERT_TRUE(out.brake_light);

    set_pedal(49u);
    step();
    assert_cut_ends(VCU_CUT_BRAKE_SENSOR);
    TEST_ASSERT_FALSE(out.brake_light);
}

/* Spec #36: the brake light is on whenever a brake sensor has failed, from
   its first step, before the 80 ms that make it a Torque cut. */
static void test_a_failed_brake_sensor_lights_the_brake_light_at_once(void)
{
    driving();
    in.raw.brake[0] = 3800u;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
    TEST_ASSERT_TRUE(out.brake_light);
}

/* T.4.3.3 across the 32-bit millisecond wrap. */
static void test_the_brake_sensor_timer_works_across_the_clock_wrap(void)
{
    in.now_ms = 0xFFFFFF90u;
    driving();
    TEST_ASSERT_TRUE(in.now_ms > 0xFFFFFF00u); /* the wrap is still ahead */
    in.raw.brake[1] = 10u;
    run_ms(80u);
    TEST_ASSERT_TRUE(in.now_ms < 0x100u);
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);
    step();
    assert_cut_starts(VCU_CUT_BRAKE_SENSOR);
}

/* Spec #36: the brake light is on for a failed brake sensor in any state. */
static void test_a_failed_brake_sensor_lights_the_brake_light_in_glv_on(void)
{
    to_glv_on();
    in.raw.brake[1] = 3800u;
    run_ms(100u);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
    TEST_ASSERT_TRUE(out.brake_light);
}

/* EV.4.7: Brake engaged with more than 25% travel cuts torque in the same
   step; exactly 25% does not. */
static void test_brake_with_the_pedal_above_25_percent_cuts_torque_at_once(void)
{
    driving();
    set_pedal(250u);
    brake_on();
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, out.cuts);

    set_pedal(251u);
    step();
    assert_cut_starts(VCU_CUT_BRAKE_PEDAL);
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());
    TEST_ASSERT_EQUAL_UINT8(1u, command_frame().inverter_enable);
    TEST_ASSERT_TRUE(out.test_point);
    step();
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().brake_pedal_cut);
}

/* EV.4.7: the 25% check uses the higher Pedal travel. */
static void test_the_25_percent_check_uses_the_higher_travel(void)
{
    driving();
    brake_on();
    set_pedals(260u, 200u); /* plausible; only APPS1 above 25% */
    step();
    assert_cut_starts(VCU_CUT_BRAKE_PEDAL);
}

/* EV.4.7: power stays off until the APPS read below 5%, whether or not the
   brake is still on; releasing the brake alone does not restore it. */
static void test_a_brake_pedal_cut_recovers_below_5_percent_with_the_brake_held(void)
{
    driving();
    brake_on();
    set_pedal(600u);
    step();
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_BRAKE_PEDAL, out.cuts);

    brake_off();
    run_ms(500u);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_BRAKE_PEDAL, out.cuts);
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());

    brake_on();
    set_pedal(49u);
    step();
    assert_cut_ends(VCU_CUT_BRAKE_PEDAL);
}

/* Spec #36: three Overruns within a second are a Torque cut ("timing
   fault"), held until the pedal is below 5%. */
static void test_a_timing_fault_cuts_torque_until_below_5_percent(void)
{
    driving();
    in.timing_fault = true; /* true for the one step after the third */
    step();
    in.timing_fault = false;
    assert_cut_starts(VCU_CUT_TIMING);
    TEST_ASSERT_EQUAL_INT16(0, torque_sent());
    TEST_ASSERT_EQUAL_UINT8(1u, command_frame().inverter_enable);
    run_ms(500u);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_TIMING, out.cuts);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().timing_fault);

    set_pedal(49u);
    step();
    assert_cut_ends(VCU_CUT_TIMING);
}

/* ------------------------------------------------------------------ */
/* The fault LED: blinking for a Torque cut, solid for a restart      */
/* ------------------------------------------------------------------ */

/* ADR 0006: the fault LED blinks while a Torque cut lasts, from the step
   that zeroes the command. */
static void test_the_fault_led_blinks_during_a_torque_cut(void)
{
    in.now_ms = 10000u; /* well past the lamp check */
    driving();
    TEST_ASSERT_FALSE(out.fault_led);

    brake_on();
    set_pedal(600u);
    step(); /* the cut starts */
    TEST_ASSERT_TRUE(out.fault_led);
    for (uint32_t t = STEP_MS; t < VCU_FAULT_BLINK_MS; t += STEP_MS) {
        step();
        TEST_ASSERT_TRUE(out.fault_led);
    }
    for (uint32_t t = 0u; t < VCU_FAULT_BLINK_MS; t += STEP_MS) {
        step();
        TEST_ASSERT_FALSE(out.fault_led);
    }
    step();
    TEST_ASSERT_TRUE(out.fault_led);

    set_pedal(0u);
    step(); /* the cut ends */
    TEST_ASSERT_FALSE(out.fault_led);
    TEST_ASSERT_FALSE(out.fault_active);
}

/* ADR 0006, issue 47: Leaving Ready to Drive lights the fault LED solid and
   sets fault_active and the test point in the step that disables the
   inverter, until the Start procedure is done again. */
static void test_leaving_ready_to_drive_lights_the_fault_led_solid(void)
{
    in.now_ms = 10000u;
    driving();
    TEST_ASSERT_FALSE(out.fault_active);
    in.sdc_closed = false;
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, command_frame().inverter_enable);
    TEST_ASSERT_TRUE(out.test_point);
    TEST_ASSERT_TRUE(out.fault_active);
    TEST_ASSERT_TRUE(out.fault_led);

    in.sdc_closed = true;
    set_pedal(0u);
    for (int i = 0; i < 100; i++) {
        step();
        TEST_ASSERT_TRUE(out.fault_led); /* solid, never blinking */
        TEST_ASSERT_TRUE(out.fault_active);
    }

    brake_on();
    press_start();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
    TEST_ASSERT_FALSE(out.fault_led);
    TEST_ASSERT_FALSE(out.fault_active);
    TEST_ASSERT_FALSE(out.test_point);
}

/* Back in GLV_ON (the bus discharged) there is nothing to restart. */
static void test_the_restart_light_goes_out_in_glv_on(void)
{
    in.now_ms = 10000u;
    driving();
    in.sdc_closed = false;
    step();
    TEST_ASSERT_TRUE(out.fault_led);
    set_bus_dv(0);
    step();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_GLV_ON, out.state);
    TEST_ASSERT_FALSE(out.fault_led);
    TEST_ASSERT_FALSE(out.fault_active);
}

/* A restart needed wins over a Torque cut's blink. */
static void test_the_restart_light_is_solid_through_a_torque_cut(void)
{
    in.now_ms = 10000u;
    driving();
    invFaults.run_faults = 1u;
    in.timing_fault = true;
    step();
    in.timing_fault = false;
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_TIMING, out.cuts);
    for (int i = 0; i < 60; i++) {
        step();
        TEST_ASSERT_TRUE(out.fault_led);
    }
}

/* ------------------------------------------------------------------ */
/* The fault LED and VCU_Status                                       */
/* ------------------------------------------------------------------ */

static void test_the_fault_led_is_on_in_startup(void)
{
    in.startup_ok = false;
    in.now_ms = 10000u;
    step();
    TEST_ASSERT_TRUE(out.fault_led);
}

/* Spec #36: the fault LED is held 3 s after every reset. */
static void test_the_fault_led_is_held_3_s_after_reset(void)
{
    to_glv_on();
    TEST_ASSERT_TRUE(out.fault_led);
    while (in.now_ms < VCU_LAMP_CHECK_MS) {
        TEST_ASSERT_TRUE(out.fault_led);
        step();
    }
    TEST_ASSERT_TRUE(out.fault_led); /* the step at 2990 ms */
    step();                          /* 3000 ms */
    TEST_ASSERT_FALSE(out.fault_led);
}

static void test_the_fault_led_lamp_check_ends_across_the_clock_wrap(void)
{
    config.reset_ms = 0xFFFFF000u; /* 4096 ms before the wrap */
    vcu_control_init(&vc, &config);
    in.now_ms = 0xFFFFF000u;
    to_glv_on();
    run_ms(VCU_LAMP_CHECK_MS);
    TEST_ASSERT_FALSE(out.fault_led);
    run_ms(5000u); /* past the wrap: still off */
    TEST_ASSERT_FALSE(out.fault_led);
}

static void test_vcu_status_reports_the_last_reset_and_the_flash(void)
{
    config.last_reset_not_power_on = true;
    config.not_dual_bank = true;
    vcu_control_init(&vc, &config);
    step();
    const struct can_control_vcu_status_t st = status_frame();
    TEST_ASSERT_EQUAL_UINT8(1u, st.last_reset_not_power_on);
    TEST_ASSERT_EQUAL_UINT8(1u, st.not_dual_bank);
}

static void test_vcu_status_reports_the_start_button(void)
{
    to_glv_on();
    in.start_pressed = true;
    run_ms(20u);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().start_button);
}

static void test_vcu_status_goes_every_second_step_with_its_e2e_check(void)
{
    e2e_p11_rx_t check = {0};
    for (int i = 0; i < 10; i++) {
        step();
        TEST_ASSERT_EQUAL(i % 2 == 0, out.send_status);
        if (out.send_status) {
            TEST_ASSERT_EQUAL(E2E_P11_OK, e2e_p11_check(&check, 0x0090u, out.status, 8u));
        }
    }
}

/* ------------------------------------------------------------------ */
/* The save and bench refusal rules                                   */
/* ------------------------------------------------------------------ */

/* ADR 0015: a Parameter save only in GLV_ON. */
static void test_a_parameter_save_is_allowed_only_in_glv_on(void)
{
    TEST_ASSERT_NULL(vcu_control_save_refusal(true, VCU_STATE_GLV_ON));
    TEST_ASSERT_NOT_NULL(vcu_control_save_refusal(false, VCU_STATE_GLV_ON));
    TEST_ASSERT_NOT_NULL(vcu_control_save_refusal(true, VCU_STATE_STARTUP));
    TEST_ASSERT_NOT_NULL(vcu_control_save_refusal(true, VCU_STATE_TS_ACTIVE));
    TEST_ASSERT_NOT_NULL(vcu_control_save_refusal(true, VCU_STATE_READY_TO_DRIVE));
}

/* ADR 0002 amendment: `hang` and `crash` are refused in READY_TO_DRIVE. */
static void test_the_bench_commands_are_refused_only_in_ready_to_drive(void)
{
    TEST_ASSERT_NOT_NULL(vcu_control_bench_refusal(true, VCU_STATE_READY_TO_DRIVE));
    TEST_ASSERT_NULL(vcu_control_bench_refusal(true, VCU_STATE_TS_ACTIVE));
    TEST_ASSERT_NULL(vcu_control_bench_refusal(true, VCU_STATE_GLV_ON));
    TEST_ASSERT_NULL(vcu_control_bench_refusal(false, VCU_STATE_STARTUP));
}

static void test_state_names(void)
{
    TEST_ASSERT_EQUAL_STRING("STARTUP", vcu_control_state_name(VCU_STATE_STARTUP));
    TEST_ASSERT_EQUAL_STRING("GLV_ON", vcu_control_state_name(VCU_STATE_GLV_ON));
    TEST_ASSERT_EQUAL_STRING("TS_ACTIVE", vcu_control_state_name(VCU_STATE_TS_ACTIVE));
    TEST_ASSERT_EQUAL_STRING("READY_TO_DRIVE", vcu_control_state_name(VCU_STATE_READY_TO_DRIVE));
    TEST_ASSERT_EQUAL_STRING("?", vcu_control_state_name(9u));
}

/* ------------------------------------------------------------------ */
/* Calibrated (issue 59; ADR 0015, 0016)                              */
/* ------------------------------------------------------------------ */

/* The first Event of this step with code, or NULL. */
static const vcu_event_t *find_event(uint16_t code)
{
    for (size_t i = 0; i < out.event_count; i++) {
        if (out.events[i].code == code) {
            return &out.events[i];
        }
    }
    return NULL;
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

/* ADR 0015: not calibrated, the VCU still reaches GLV_ON (the shell is
   needed to calibrate) and writes an Event with the reason. */
static void test_not_calibrated_still_reaches_glv_on_and_writes_an_event(void)
{
    config.cal_revision = VCU_CALIBRATION_REVISION + 1u; /* another sensor path */
    vcu_control_init(&vc, &config);
    step();
    assert_transition(VCU_STATE_STARTUP, VCU_STATE_GLV_ON, VCU_CAUSE_STARTUP_DONE);

    const vcu_event_t *ev = find_event(EVENT_NOT_CALIBRATED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(VCU_CAL_REVISION, ev->data[0]);
    TEST_ASSERT_EQUAL_UINT8(VCU_CALIBRATION_REVISION + 1u, ev->data[2]); /* stored */
    TEST_ASSERT_EQUAL_UINT8(VCU_CALIBRATION_REVISION, ev->data[4]);      /* the image's */

    step();
    TEST_ASSERT_NULL(find_event(EVENT_NOT_CALIBRATED)); /* once */
}

static void test_a_calibrated_vcu_writes_no_not_calibrated_event(void)
{
    step();
    TEST_ASSERT_NULL(find_event(EVENT_NOT_CALIBRATED));
    TEST_ASSERT_EQUAL_INT(VCU_CAL_OK, out.calibration.reason);
}

/* Each part of the Calibrated rule reaches the step: a span too small. */
static void test_a_calibration_failing_the_rule_is_not_calibrated(void)
{
    config.cal.apps_max[1] = APPS_MIN + 700; /* under the 744-count minimum span */
    vcu_control_init(&vc, &config);
    step();
    TEST_ASSERT_EQUAL_INT(VCU_CAL_APPS_SPAN, out.calibration.reason);
    TEST_ASSERT_EQUAL_INT(VCU_CAL_SENSOR_APPS2, out.calibration.sensor);
}

static void test_not_calibrated_treats_the_pedals_as_implausible(void)
{
    config.cal_present = false;
    vcu_control_init(&vc, &config);
    set_pedal(0u); /* agreeing, in range */
    to_glv_on();
    TEST_ASSERT_EQUAL(VCU_SENSOR_NOT_CALIBRATED, out.sensors.apps[0].status);
    TEST_ASSERT_EQUAL(VCU_SENSOR_NOT_CALIBRATED, out.sensors.apps[1].status);
    run_ms(VCU_CUT_PERSIST_MS);
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_APPS, out.cuts);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().apps_implausible);
}

static void test_start_is_refused_when_not_calibrated(void)
{
    config.cal_present = false;
    vcu_control_init(&vc, &config);
    to_ts_active();
    brake_on();
    assert_start_refused(VCU_START_NOT_CALIBRATED);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().not_calibrated);
}

static void test_vcu_status_reports_calibrated(void)
{
    step();
    TEST_ASSERT_EQUAL_UINT8(0u, status_frame().not_calibrated);
    TEST_ASSERT_BITS_LOW(VCU_START_NOT_CALIBRATED, out.start_blockers);
}

/* ADR 0015: not calibrated, or parameters defaulted, shows the fault LED
   past the lamp check, steadily. */
static void test_the_fault_led_shows_when_not_calibrated(void)
{
    config.cal_present = false;
    vcu_control_init(&vc, &config);
    to_glv_on();
    run_ms(VCU_LAMP_CHECK_MS);
    for (unsigned i = 0; i < 100u; i++) {
        step();
        TEST_ASSERT_TRUE(out.fault_led);
    }
}

static void test_parameters_defaulted_treats_the_pedals_as_implausible_with_the_fault_led(void)
{
    config.params_defaulted = true; /* the calibration itself loaded fine */
    vcu_control_init(&vc, &config);
    to_glv_on();
    TEST_ASSERT_EQUAL(VCU_SENSOR_NOT_CALIBRATED, out.sensors.apps[0].status);
    run_ms(VCU_LAMP_CHECK_MS);
    for (unsigned i = 0; i < 100u; i++) {
        step();
        TEST_ASSERT_TRUE(out.fault_led);
    }
    TEST_ASSERT_BITS_HIGH(1u << VCU_CUT_APPS, out.cuts);
}

/* ------------------------------------------------------------------ */
/* The Pedal check (issue 59; ADR 0016)                               */
/* ------------------------------------------------------------------ */

/* Both APPS from one travel to another in 1% steps, one step each. */
static void sweep_pedal(uint16_t from, uint16_t to)
{
    uint16_t t = from;

    for (;;) {
        set_pedal(t);
        step();
        if (t == to) {
            break;
        }
        if (to > t) {
            t = (uint16_t)(((unsigned)(to - t) > 10u) ? t + 10u : to);
        } else {
            t = (uint16_t)(((unsigned)(t - to) > 10u) ? t - 10u : to);
        }
    }
}

/* In GLV_ON with no matching pass in the Event log. */
static void check_due_in_glv_on(void)
{
    in.pedal_check_record = VCU_PEDAL_CHECK_ABSENT;
    set_pedal(0u);
    brake_off();
    to_glv_on();
    TEST_ASSERT_TRUE(out.pedal_check_due);
}

/* Released, floored, released, then the brake engaged. True if any of it
   wrote a pass. */
static bool do_pedal_check(uint16_t floor)
{
    const unsigned before = passes;

    brake_off();
    sweep_pedal(0u, floor);
    sweep_pedal(floor, 0u);
    brake_on();
    step();
    return passes > before;
}

static void test_start_is_refused_while_a_pedal_check_is_due(void)
{
    in.pedal_check_record = VCU_PEDAL_CHECK_ABSENT;
    to_ts_active();
    brake_on();
    assert_start_refused(VCU_START_PEDAL_CHECK_DUE);
    TEST_ASSERT_EQUAL_UINT8(1u, status_frame().pedal_check_due);
}

/* Until the Event log has been searched, a check is due. */
static void test_a_pedal_check_is_due_until_the_event_log_is_searched(void)
{
    in.pedal_check_record = VCU_PEDAL_CHECK_UNKNOWN;
    to_ts_active();
    brake_on();
    assert_start_refused(VCU_START_PEDAL_CHECK_DUE);
}

static void test_a_matching_pass_in_the_event_log_needs_no_check(void)
{
    to_glv_on();
    TEST_ASSERT_FALSE(out.pedal_check_due);
    TEST_ASSERT_EQUAL_UINT8(0u, status_frame().pedal_check_due);
}

/* ADR 0016: a pass writes an Event with the build ID and the parameter
   save counter, and the Start procedure is then accepted. */
static void test_a_pedal_check_pass_writes_its_event_and_allows_the_start(void)
{
    check_due_in_glv_on();
    TEST_ASSERT_TRUE(do_pedal_check(1000u));

    const vcu_event_t *ev = find_event(EVENT_PEDAL_CHECK); /* on the brake step */
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT32(BUILD_ID, get_u32(&ev->data[0]));
    TEST_ASSERT_EQUAL_UINT32(SAVE_COUNTER, get_u32(&ev->data[4]));
    TEST_ASSERT_EQUAL_UINT8(0u, ev->data[8]); /* not a dirty build */
    const vcu_cal_values_t cal = {
        .apps_min = { APPS_MIN, APPS_MIN },
        .apps_max = { APPS_MAX, APPS_MAX },
        .brake_zero = { BRAKE_ZERO, BRAKE_ZERO },
        .brake_engaged = BRAKE_THRESH,
        .revision = VCU_CALIBRATION_REVISION,
    };
    TEST_ASSERT_EQUAL_UINT32(vcu_cal_fingerprint(&cal), get_u32(&ev->data[12]));
    TEST_ASSERT_FALSE(out.pedal_check_due);

    step();
    TEST_ASSERT_NULL(find_event(EVENT_PEDAL_CHECK)); /* once */

    play_all(true);
    run_ms(3u * STEP_MS);
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_TS_ACTIVE, out.state);
    press_start();
    TEST_ASSERT_EQUAL_UINT8(VCU_STATE_READY_TO_DRIVE, out.state);
}

static void test_a_dirty_build_marks_its_pass(void)
{
    config.build_dirty = true;
    vcu_control_init(&vc, &config);
    check_due_in_glv_on();
    TEST_ASSERT_TRUE(do_pedal_check(1000u));
    TEST_ASSERT_EQUAL_UINT8(1u, find_event(EVENT_PEDAL_CHECK)->data[8]);
}

/* Floored is both above 95%: 95.0% is not enough. */
static void test_floored_means_both_above_95_percent(void)
{
    check_due_in_glv_on();
    TEST_ASSERT_FALSE(do_pedal_check(950u));
    TEST_ASSERT_TRUE(out.pedal_check_due);
    TEST_ASSERT_TRUE(do_pedal_check(951u));
}

/* Released is both below 5%: a pedal that comes back only to 5.0% has
   not been released. */
static void test_released_means_both_below_5_percent(void)
{
    check_due_in_glv_on();
    brake_off();
    sweep_pedal(0u, 1000u);
    sweep_pedal(1000u, 50u);
    brake_on();
    run_ms(100u);
    TEST_ASSERT_EQUAL_UINT(0u, passes);
    set_pedal(49u);
    step();
    TEST_ASSERT_NOT_NULL(find_event(EVENT_PEDAL_CHECK));
}

/* The two sensors within 10 points throughout: 10.0 apart is fine, 10.1
   fails the attempt with an Event, and the check starts over. */
static void test_sensors_more_than_10_points_apart_fail_the_check(void)
{
    check_due_in_glv_on();
    brake_off();
    sweep_pedal(0u, 500u);
    set_pedals(500u, 600u);
    step();
    TEST_ASSERT_NULL(find_event(EVENT_PEDAL_CHECK_FAILED));
    set_pedals(500u, 601u);
    step();
    const vcu_event_t *ev = find_event(EVENT_PEDAL_CHECK_FAILED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(VCU_PEDAL_CHECK_FAIL_APART, ev->data[0]);

    /* The rest of the press no longer counts: floored and released, the
       brake engaged, and still no pass. */
    sweep_pedal(500u, 1000u);
    sweep_pedal(1000u, 0u);
    brake_on();
    step();
    TEST_ASSERT_EQUAL_UINT(0u, passes);
    TEST_ASSERT_TRUE(out.pedal_check_due);

    /* A whole new check passes. */
    brake_off();
    step();
    TEST_ASSERT_TRUE(do_pedal_check(1000u));
}

static void test_a_failed_pedal_sensor_fails_the_check(void)
{
    check_due_in_glv_on();
    sweep_pedal(0u, 500u);
    in.raw.apps[0] = 50u; /* open circuit */
    step();
    const vcu_event_t *ev = find_event(EVENT_PEDAL_CHECK_FAILED);
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL_UINT8(VCU_PEDAL_CHECK_FAIL_SENSOR, ev->data[0]);
}

/* Before the pedal leaves released nothing is under way, so nothing
   fails: no Event per step for a pedal sitting at rest. */
static void test_nothing_fails_before_the_check_is_under_way(void)
{
    check_due_in_glv_on();
    in.raw.apps[0] = 50u;
    run_ms(500u);
    TEST_ASSERT_NULL(find_event(EVENT_PEDAL_CHECK_FAILED));
}

/* The brake must read released and then engaged: a brake held on through
   the whole check has not been seen released. */
static void test_the_brake_must_be_released_then_engaged(void)
{
    check_due_in_glv_on();
    brake_on();
    step();
    sweep_pedal(0u, 1000u);
    sweep_pedal(1000u, 0u);
    run_ms(100u);
    TEST_ASSERT_EQUAL_UINT(0u, passes);

    brake_off();
    step();
    TEST_ASSERT_TRUE(out.pedal_check_due);
    brake_on();
    step();
    TEST_ASSERT_NOT_NULL(find_event(EVENT_PEDAL_CHECK));
}

/* ADR 0016: with the Tractive System off. In TS_ACTIVE it does not run. */
static void test_the_pedal_check_runs_only_with_the_tractive_system_off(void)
{
    in.pedal_check_record = VCU_PEDAL_CHECK_ABSENT;
    to_ts_active();
    TEST_ASSERT_FALSE(do_pedal_check(1000u));
    TEST_ASSERT_TRUE(out.pedal_check_due);
}

/* There is nothing to confirm without a calibration. */
static void test_the_pedal_check_needs_a_calibration(void)
{
    config.cal_present = false;
    vcu_control_init(&vc, &config);
    check_due_in_glv_on();
    TEST_ASSERT_FALSE(do_pedal_check(1000u));
    TEST_ASSERT_TRUE(out.pedal_check_due);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_not_calibrated_still_reaches_glv_on_and_writes_an_event);
    RUN_TEST(test_a_calibrated_vcu_writes_no_not_calibrated_event);
    RUN_TEST(test_a_calibration_failing_the_rule_is_not_calibrated);
    RUN_TEST(test_not_calibrated_treats_the_pedals_as_implausible);
    RUN_TEST(test_start_is_refused_when_not_calibrated);
    RUN_TEST(test_vcu_status_reports_calibrated);
    RUN_TEST(test_the_fault_led_shows_when_not_calibrated);
    RUN_TEST(test_parameters_defaulted_treats_the_pedals_as_implausible_with_the_fault_led);
    RUN_TEST(test_start_is_refused_while_a_pedal_check_is_due);
    RUN_TEST(test_a_pedal_check_is_due_until_the_event_log_is_searched);
    RUN_TEST(test_a_matching_pass_in_the_event_log_needs_no_check);
    RUN_TEST(test_a_pedal_check_pass_writes_its_event_and_allows_the_start);
    RUN_TEST(test_a_dirty_build_marks_its_pass);
    RUN_TEST(test_floored_means_both_above_95_percent);
    RUN_TEST(test_released_means_both_below_5_percent);
    RUN_TEST(test_sensors_more_than_10_points_apart_fail_the_check);
    RUN_TEST(test_a_failed_pedal_sensor_fails_the_check);
    RUN_TEST(test_nothing_fails_before_the_check_is_under_way);
    RUN_TEST(test_the_brake_must_be_released_then_engaged);
    RUN_TEST(test_the_pedal_check_runs_only_with_the_tractive_system_off);
    RUN_TEST(test_the_pedal_check_needs_a_calibration);
    RUN_TEST(test_startup_goes_to_glv_on_on_the_first_step_with_everything_ready);
    RUN_TEST(test_startup_stays_while_a_task_or_queue_is_missing);
    RUN_TEST(test_startup_waits_for_can1_and_a_full_set_of_samples);
    RUN_TEST(test_startup_ends_with_parameters_defaulted);
    RUN_TEST(test_startup_sends_no_inverter_command);
    RUN_TEST(test_glv_on_goes_to_ts_active_once_every_mailbox_is_valid);
    RUN_TEST(test_glv_on_stays_with_the_shutdown_circuit_open);
    RUN_TEST(test_glv_on_stays_while_the_bms_reports_a_fault);
    RUN_TEST(test_glv_on_stays_while_a_bms_fault_bit_is_active);
    RUN_TEST(test_glv_on_stays_with_any_bms_or_inverter_mailbox_silent);
    RUN_TEST(test_glv_on_does_not_need_the_logged_only_frames);
    RUN_TEST(test_glv_on_needs_the_bus_at_90_percent_of_the_pack);
    RUN_TEST(test_glv_on_needs_the_bus_at_60_v_whatever_the_pack_says);
    RUN_TEST(test_ts_active_returns_to_glv_on_below_60_v);
    RUN_TEST(test_ts_active_lasts_while_the_bus_discharges);
    RUN_TEST(test_a_silent_inverter_does_not_end_ts_active);
    RUN_TEST(test_brake_engaged_is_above_the_threshold_not_at_it);
    RUN_TEST(test_brake_engaged_releases_only_below_its_hysteresis);
    RUN_TEST(test_either_brake_sensor_alone_engages_the_brake);
    RUN_TEST(test_a_failed_brake_sensor_does_not_engage_the_brake);
    RUN_TEST(test_the_brake_light_follows_the_brake_in_startup);
    RUN_TEST(test_the_brake_light_follows_the_brake_in_ts_active);
    RUN_TEST(test_the_start_procedure_enters_ready_to_drive);
    RUN_TEST(test_a_button_held_before_the_brake_does_not_start);
    RUN_TEST(test_the_brake_and_the_button_in_the_same_step_do_not_start);
    RUN_TEST(test_a_bounce_shorter_than_the_debounce_is_not_a_press);
    RUN_TEST(test_a_button_held_since_reset_never_counts);
    RUN_TEST(test_start_is_refused_in_glv_on);
    RUN_TEST(test_start_is_refused_without_the_brake);
    RUN_TEST(test_start_is_refused_with_the_pedal_at_5_percent);
    RUN_TEST(test_start_is_refused_with_one_pedal_sensor_pressed);
    RUN_TEST(test_start_is_refused_with_a_pedal_sensor_failed);
    RUN_TEST(test_start_is_refused_with_a_brake_sensor_failed);
    RUN_TEST(test_start_is_refused_with_parameters_defaulted);
    RUN_TEST(test_start_is_refused_with_a_mailbox_not_valid);
    RUN_TEST(test_start_is_refused_while_the_bms_reports_a_fault);
    RUN_TEST(test_start_is_refused_while_the_inverter_reports_a_fault);
    RUN_TEST(test_start_is_refused_with_the_shutdown_circuit_open);
    RUN_TEST(test_start_is_refused_with_the_bus_off_latch_set);
    RUN_TEST(test_start_is_refused_with_the_bus_below_90_percent);
    RUN_TEST(test_start_is_refused_on_a_timing_fault);
    RUN_TEST(test_ready_to_drive_is_left_when_the_shutdown_circuit_opens);
    RUN_TEST(test_ready_to_drive_is_left_when_the_bms_reports_a_fault);
    RUN_TEST(test_ready_to_drive_is_left_when_the_inverter_reports_a_fault);
    RUN_TEST(test_ready_to_drive_is_left_when_the_bus_off_latch_sets);
    RUN_TEST(test_ready_to_drive_is_left_below_80_percent_of_the_pack);
    RUN_TEST(test_leaving_ready_to_drive_records_every_cause_that_applied);
    RUN_TEST(test_a_silent_bms_does_not_leave_ready_to_drive);
    RUN_TEST(test_a_silent_inverter_does_not_leave_ready_to_drive);
    RUN_TEST(test_after_leaving_the_start_procedure_is_needed_again);
    RUN_TEST(test_the_sound_runs_2_s_from_entry);
    RUN_TEST(test_the_sound_stops_at_once_when_ready_to_drive_is_left);
    RUN_TEST(test_there_is_no_sound_outside_ready_to_drive);
    RUN_TEST(test_the_sound_times_2_s_across_the_clock_wrap);
    RUN_TEST(test_the_start_procedure_works_across_the_clock_wrap);
    RUN_TEST(test_the_command_goes_every_step_after_startup_with_enable_0);
    RUN_TEST(test_the_command_is_sent_in_ts_active_with_enable_0);
    RUN_TEST(test_the_rolling_counter_counts_every_frame_modulo_16);
    RUN_TEST(test_the_first_command_in_ready_to_drive_enables_with_zero_torque);
    RUN_TEST(test_ready_to_drive_keeps_enable_1_and_reports_it);
    RUN_TEST(test_leaving_ready_to_drive_sends_enable_0_in_the_same_step);
    RUN_TEST(test_the_pedal_map_is_zero_below_5_percent_and_max_at_100);
    RUN_TEST(test_the_request_uses_the_lower_pedal_travel);
    RUN_TEST(test_the_command_is_never_negative);
    RUN_TEST(test_no_torque_outside_ready_to_drive);
    RUN_TEST(test_the_entry_frame_has_zero_torque_and_the_next_the_request);
    RUN_TEST(test_leaving_ready_to_drive_zeroes_torque_in_the_same_step);
    RUN_TEST(test_apps_disagreeing_for_80_ms_cuts_torque_79_ms_does_not);
    RUN_TEST(test_apps_exactly_10_points_apart_is_plausible);
    RUN_TEST(test_an_apps_out_of_range_for_80_ms_cuts_torque);
    RUN_TEST(test_an_apps_glitch_under_80_ms_restarts_the_timer);
    RUN_TEST(test_an_apps_cut_recovers_only_below_5_percent);
    RUN_TEST(test_an_apps_cut_does_not_recover_while_still_implausible);
    RUN_TEST(test_the_apps_timer_works_across_the_clock_wrap);
    RUN_TEST(test_a_brake_sensor_failed_for_80_ms_cuts_torque);
    RUN_TEST(test_a_brake_sensor_cut_recovers_only_below_5_percent);
    RUN_TEST(test_a_failed_brake_sensor_lights_the_brake_light_at_once);
    RUN_TEST(test_the_brake_sensor_timer_works_across_the_clock_wrap);
    RUN_TEST(test_a_failed_brake_sensor_lights_the_brake_light_in_glv_on);
    RUN_TEST(test_brake_with_the_pedal_above_25_percent_cuts_torque_at_once);
    RUN_TEST(test_the_25_percent_check_uses_the_higher_travel);
    RUN_TEST(test_a_brake_pedal_cut_recovers_below_5_percent_with_the_brake_held);
    RUN_TEST(test_a_timing_fault_cuts_torque_until_below_5_percent);
    RUN_TEST(test_the_fault_led_blinks_during_a_torque_cut);
    RUN_TEST(test_leaving_ready_to_drive_lights_the_fault_led_solid);
    RUN_TEST(test_the_restart_light_goes_out_in_glv_on);
    RUN_TEST(test_the_restart_light_is_solid_through_a_torque_cut);
    RUN_TEST(test_the_fault_led_is_on_in_startup);
    RUN_TEST(test_the_fault_led_is_held_3_s_after_reset);
    RUN_TEST(test_the_fault_led_lamp_check_ends_across_the_clock_wrap);
    RUN_TEST(test_vcu_status_reports_the_last_reset_and_the_flash);
    RUN_TEST(test_vcu_status_reports_the_start_button);
    RUN_TEST(test_vcu_status_goes_every_second_step_with_its_e2e_check);
    RUN_TEST(test_a_parameter_save_is_allowed_only_in_glv_on);
    RUN_TEST(test_the_bench_commands_are_refused_only_in_ready_to_drive);
    RUN_TEST(test_state_names);
    return UNITY_END();
}
