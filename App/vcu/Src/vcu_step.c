/* vcu_step.c — the VCU's Control step, see vcu_step.h. */

#include "vcu_step.h"

#include <stddef.h>
#include <string.h>

#include "build_id.h"
#include "bus_term.h"
#include "control_bus.h"
#include "event_codes.h"
#include "events.h"
#include "flash_bank2.h"
#include "main.h"
#include "parameters.h"
#include "reset_cause.h"
#include "reset_info.h"
#include "vcu_adc.h"
#include "vcu_log.h"
#include "vcu_params.h"

#define VCU_STEP_PERIOD_MS 10u

/* The first step starts CAN1 one period after termination was written
   (control_bus_init()), so the period must outlast the settle time. */
_Static_assert(VCU_STEP_PERIOD_MS > BUS_TERM_SETTLE_MS,
               "the first step must find CAN1's termination settled");

_Static_assert(VCU_RX_COUNT <= CONTROL_BUS_RX_MAX, "the VCU's mailboxes fit control_bus");

/* Owned by the step; nothing else touches them. Static, not on the step's
   stack: the output holds two frames and the Events. */
static bool canStarted;
static vcu_control_t control;
static vcu_control_in_t in;
static vcu_control_out_t out;
static vcu_inputs_t inputs;

/* VCU_PEDAL_CHECK_...: written once by the Event writer's task, read by
   the step; one byte, loaded and stored whole, so the step takes no lock
   (ADR 0010). */
static uint8_t pedalCheckRecord = VCU_PEDAL_CHECK_UNKNOWN;

_Static_assert(sizeof(vcu_inputs_t) <= CONTROL_IMAGE_DATA_MAX,
               "the VCU inputs must fit the published snapshot");

bool vcu_step_init(void)
{
    /* Every frame the VCU reads, in vcu_rx_t order, one mailbox each. The
       BMS frames get receive FIFO 0 to themselves, so a burst of inverter
       or Energy Meter frames can never push one out. Static, like the
       config below: this runs on initTask's small stack (tasks.c). */
    static control_bus_rx_t rows[VCU_RX_COUNT];

    for (size_t i = 0; i < VCU_RX_COUNT; i++) {
        rows[i].frame = vcu_rx_frames[i];
        rows[i].fifo = (i <= VCU_RX_BMS_POWER) ? CONTROL_BUS_FIFO0 : CONTROL_BUS_FIFO1;
    }

    /* Parameters never change until a reset (ADR 0015), so they are read
       once. The tick counts from 0 at reset release, near enough for the
       3 s lamp check. */
    static vcu_control_config_t config;
    bool calPresent = true;

    for (size_t i = 0; i < VCU_PARAM_COUNT; i++) {
        if (vcu_param_table.defs[i].calibration) {
            calPresent = calPresent && parameters_from_copy(i);
        }
    }

    config = (vcu_control_config_t){
        .cal = {
            .apps_min = { parameters_value(VCU_PARAM_APPS1_MIN),
                          parameters_value(VCU_PARAM_APPS2_MIN) },
            .apps_max = { parameters_value(VCU_PARAM_APPS1_MAX),
                          parameters_value(VCU_PARAM_APPS2_MAX) },
            .brake_zero = { parameters_value(VCU_PARAM_BRAKE1_ZERO),
                            parameters_value(VCU_PARAM_BRAKE2_ZERO) },
        },
        .brake_engaged = (uint16_t)parameters_value(VCU_PARAM_BRAKE_ENGAGED),
        .cal_revision = (uint16_t)parameters_value(VCU_PARAM_CAL_REVISION),
        .cal_present = calPresent,
        .max_torque = (uint16_t)parameters_value(VCU_PARAM_MAX_TORQUE),
        .build_id = BUILD_ID,
        .build_dirty = BUILD_DIRTY != 0,
        .param_save_counter = parameters_save_counter(),
        .params_defaulted = parameters_defaulted(),
        .not_dual_bank = flash_bank2_get() == NULL,
        .last_reset_not_power_on = !reset_cause_is_power_on(reset_info_report()->cause),
        .reset_ms = 0u,
    };
    vcu_control_init(&control, &config);

    return control_bus_init(rows, VCU_RX_COUNT, parameters_value(VCU_PARAM_CAN1_TERM) != 0);
}

static void phase_input(control_step_ctx_t *ctx)
{
    /* CAN1 starts on the first step, a full period after the transceiver
       left standby and termination was written in vcu_step_init(). */
    if (ctx->step_count == 0u) {
        canStarted = control_bus_start();
    }
    /* The bus-off latch clears only with every mailbox VALID but the
       Energy Meter's (vcu_control_latch_may_clear()): judged on the last
       step's mailboxes. */
    control_bus_update(ctx->now_ms, vcu_control_latch_may_clear(control_bus_mailbox(0u)));

    const control_bus_health_t bus = control_bus_health();

    in.now_ms = ctx->now_ms;
    in.step_count = ctx->step_count;
    in.startup_ok = ctx->startup_ok;
    in.can_running = canStarted;
    vcu_adc_read_sensors(&in.raw);
    /* Both active low (issue 20): the opto pulls the monitor low while the
       loop is closed, the button pulls its pin low while pressed. */
    in.sdc_closed = HAL_GPIO_ReadPin(SDC_MON_N_GPIO_Port, SDC_MON_N_Pin) == GPIO_PIN_RESET;
    in.start_pressed = HAL_GPIO_ReadPin(START_BTN_N_GPIO_Port, START_BTN_N_Pin) == GPIO_PIN_RESET;
    in.mailboxes = control_bus_mailbox(0u); /* control_bus keeps them in row order */
    in.bus_off_latched = bus.latched;
    in.error_passive = bus.error_passive;
    in.rx_overflowed = bus.rx_overflowed;
    in.timing_fault = ctx->timing_fault;
    in.pedal_check_record = __atomic_load_n(&pedalCheckRecord, __ATOMIC_RELAXED);
}

static void phase_logic(control_step_ctx_t *ctx)
{
    vcu_control_step(&control, &in, &out);
    ctx->state = out.state;
    ctx->fault_active = out.fault_active;

    inputs = (vcu_inputs_t){
        .raw = in.raw,
        .sensors = out.sensors,
        .samples_ready = out.samples_ready,
        .sdc_closed = in.sdc_closed,
        .start_pressed = in.start_pressed,
        .brake_engaged = out.brake_engaged,
        .start_blockers = out.start_blockers,
        .cuts = out.cuts,
        .torque_request = out.torque_request,
        .torque = out.torque,
        .pedal_check_due = out.pedal_check_due,
        .pedal_check = out.pedal_check,
    };
}

static GPIO_PinState level(bool on)
{
    return on ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

/* The command, the outputs and the status frame in the same step (issue
   13, step order 8). Every output is active high (issue 55: each starts
   at "off", low). */
static void phase_output(control_step_ctx_t *ctx)
{
    (void)ctx;

    if (out.send_command) {
        (void)control_bus_send(CAN_CONTROL_INV_COMMAND_FRAME_ID,
                               CAN_CONTROL_INV_COMMAND_IS_EXTENDED != 0, out.command,
                               (uint8_t)sizeof(out.command));
    }
    if (out.send_fault_clear) {
        (void)control_bus_send(CAN_CONTROL_INV_PARAMETER_COMMAND_FRAME_ID,
                               CAN_CONTROL_INV_PARAMETER_COMMAND_IS_EXTENDED != 0,
                               out.fault_clear_frame, (uint8_t)sizeof(out.fault_clear_frame));
    }
    HAL_GPIO_WritePin(FAULT_TP_GPIO_Port, FAULT_TP_Pin, level(out.test_point));
    HAL_GPIO_WritePin(BRAKE_LIGHT_GPIO_Port, BRAKE_LIGHT_Pin, level(out.brake_light));
    HAL_GPIO_WritePin(RTD_SOUND_GPIO_Port, RTD_SOUND_Pin, level(out.sound));
    HAL_GPIO_WritePin(FAULT_LED_GPIO_Port, FAULT_LED_Pin, level(out.fault_led));

    if (out.send_status) {
        (void)control_bus_send(CAN_CONTROL_VCU_STATUS_FRAME_ID,
                               CAN_CONTROL_VCU_STATUS_IS_EXTENDED != 0, out.status,
                               (uint8_t)sizeof(out.status));
    }
    /* The Event queue never waits; a full one drops and counts (events.h). */
    for (size_t i = 0; i < out.event_count; i++) {
        (void)events_post(out.events[i].code, out.events[i].data);
    }
}

vcu_cal_result_t vcu_step_calibration(void)
{
    return control.calibration; /* set by vcu_step_init(), never after */
}

void vcu_step_find_pedal_check(void)
{
    const event_pedal_check_key_t key = {
        .build_id = BUILD_ID,
        .save_counter = parameters_save_counter(),
        .calibration = control.cal_fingerprint, /* set by vcu_step_init(), never after */
        .dirty = BUILD_DIRTY != 0,
    };
    event_t ev;
    const bool found = events_find(event_pedal_check_matches, &key, &ev);

    __atomic_store_n(&pedalCheckRecord, found ? VCU_PEDAL_CHECK_FOUND : VCU_PEDAL_CHECK_ABSENT,
                     __ATOMIC_RELAXED);
}

const char *vcu_step_save_refusal(void)
{
    control_snapshot_t snap;

    control_step_read(&snap);
    return vcu_control_save_refusal(snap.running, snap.state);
}

const char *vcu_step_bench_refusal(void)
{
    control_snapshot_t snap;

    control_step_read(&snap);
    return vcu_control_bench_refusal(snap.running, snap.state);
}

bool vcu_step_read_inputs(vcu_inputs_t *out_inputs)
{
    control_snapshot_t snap;

    control_step_read(&snap);
    if (!snap.running) {
        return false;
    }
    memcpy(out_inputs, snap.image_data.bytes, sizeof(*out_inputs));
    return true;
}

_Static_assert(VCU_LOG_STEP_SIZE <= CONTROL_STEP_RECORD_MAX,
               "the VCU's step Record fits the runner's buffer");

/* The step Record (issue 61): what this step read and decided, from the
   step's own statics, after the last phase. */
static uint16_t step_record(const data_log_step_t *step, uint8_t *buf, size_t cap,
                            uint8_t *type)
{
    if (cap < VCU_LOG_STEP_SIZE) {
        return 0u;
    }
    vcu_log_step_encode(buf, step, &in, &out);
    *type = DATA_LOG_VCU_STEP;
    return (uint16_t)VCU_LOG_STEP_SIZE;
}

static const control_phase_t vcuPhases[] = {
    { "input",  phase_input },
    { "logic",  phase_logic },
    { "output", phase_output },
};

const control_step_def_t vcu_step_def = {
    .image = "VCU",
    .period_ms = VCU_STEP_PERIOD_MS,
    .phases = vcuPhases,
    .phase_count = sizeof(vcuPhases) / sizeof(vcuPhases[0]),
    .state_name = vcu_control_state_name,
    .image_data = &inputs,
    .image_data_size = sizeof(inputs),
    .record = step_record,
};
