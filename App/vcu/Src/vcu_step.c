/* vcu_step.c — the VCU's Control step, see vcu_step.h. */

#include "vcu_step.h"

#include <stddef.h>
#include <string.h>

#include "adc_window.h"
#include "can_control.h"
#include "control_bus.h"
#include "e2e_p11.h"
#include "main.h"
#include "parameters.h"
#include "vcu_adc.h"
#include "vcu_params.h"
#include "vcu_tx.h"

#define VCU_STEP_PERIOD_MS 10u

/* Every frame the VCU reads (the receivers in can/control.dbc), one
   mailbox each. The BMS frames get receive FIFO 0 to themselves, so a
   burst of inverter or Energy Meter frames can never push one out. */
static const control_bus_rx_t vcuRx[] = {
    { CONTROL_RX_BMS_STATUS,            CONTROL_BUS_FIFO0 },
    { CONTROL_RX_BMS_LIMITS,            CONTROL_BUS_FIFO0 },
    { CONTROL_RX_BMS_POWER,             CONTROL_BUS_FIFO0 },
    { CONTROL_RX_INV_MOTOR_POSITION,    CONTROL_BUS_FIFO1 },
    { CONTROL_RX_INV_VOLTAGE,           CONTROL_BUS_FIFO1 },
    { CONTROL_RX_INV_INTERNAL_STATES,   CONTROL_BUS_FIFO1 },
    { CONTROL_RX_INV_FAULT_CODES,       CONTROL_BUS_FIFO1 },
    { CONTROL_RX_INV_TORQUE_CAPABILITY, CONTROL_BUS_FIFO1 },
    { CONTROL_RX_EM_MEASUREMENT,        CONTROL_BUS_FIFO1 },
    { CONTROL_RX_EM_STATUS,             CONTROL_BUS_FIFO1 },
    { CONTROL_RX_IMD_INFO_GENERAL,      CONTROL_BUS_FIFO1 },
};

/* Owned by the step; nothing else touches them. */
static bool canStarted;
static e2e_p11_tx_t statusTx;
static vcu_inputs_t inputs;

_Static_assert(sizeof(vcu_inputs_t) <= CONTROL_IMAGE_DATA_MAX,
               "the VCU inputs must fit the published snapshot");

/* The calibration in force: Parameters never change until a reset
   (ADR 0015), so they are read once. */
static vcu_sensor_cal_t calibration;

bool vcu_step_init(void)
{
    calibration = (vcu_sensor_cal_t){
        .apps_min = { parameters_value(VCU_PARAM_APPS1_MIN),
                      parameters_value(VCU_PARAM_APPS2_MIN) },
        .apps_max = { parameters_value(VCU_PARAM_APPS1_MAX),
                      parameters_value(VCU_PARAM_APPS2_MAX) },
        .brake_zero = { parameters_value(VCU_PARAM_BRAKE1_ZERO),
                        parameters_value(VCU_PARAM_BRAKE2_ZERO) },
    };
    return control_bus_init(vcuRx, sizeof(vcuRx) / sizeof(vcuRx[0]));
}

static bool all_sampled(const vcu_sensor_raw_t *raw)
{
    const uint16_t all[] = { raw->apps[0],  raw->apps[1],   raw->brake[0],
                             raw->brake[1], raw->supply[0], raw->supply[1] };

    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (all[i] == ADC_WINDOW_EMPTY) {
            return false;
        }
    }
    return true;
}

static void read_inputs(void)
{
    vcu_adc_read_sensors(&inputs.raw);
    vcu_sensors_compute(&inputs.raw, &calibration, &inputs.sensors);
    inputs.samples_ready = all_sampled(&inputs.raw);
    /* Both active low (issue 20): the opto pulls the monitor low while the
       loop is closed, the button pulls its pin low while pressed. */
    inputs.sdc_closed = HAL_GPIO_ReadPin(SDC_MON_N_GPIO_Port, SDC_MON_N_Pin) == GPIO_PIN_RESET;
    inputs.start_pressed =
        HAL_GPIO_ReadPin(START_BTN_N_GPIO_Port, START_BTN_N_Pin) == GPIO_PIN_RESET;
}

static void phase_input(control_step_ctx_t *ctx)
{
    /* CAN1 starts on the first step, a full period after the transceiver
       left standby in vcu_step_init(). */
    if (ctx->step_count == 0u) {
        canStarted = control_bus_start();
    }
    /* The bus-off latch clears only with every mailbox VALID (spec,
       reaction table): judged on the last step's mailboxes. */
    control_bus_update(ctx->now_ms, control_bus_all_valid());

    read_inputs();
}

static void phase_logic(control_step_ctx_t *ctx)
{
    /* No logic yet: the VCU stays in STARTUP. A blocked start-up (a task
       or queue missing, or CAN1 refused) is the only fault so far: the
       heartbeat shows it at 5 Hz. Torque cuts and Leaving Ready to Drive
       join it with their logic (#57, #58). */
    ctx->state = CAN_CONTROL_VCU_STATUS_STATE_STARTUP_CHOICE;
    ctx->fault_active = !ctx->startup_ok || !canStarted;
}

static void phase_output(control_step_ctx_t *ctx)
{
    if (vcu_tx_status_due(ctx->step_count)) {
        const control_bus_health_t bus = control_bus_health();
        struct can_control_vcu_status_t status = {
            .state = ctx->state,
            .startup_blocked = (!ctx->startup_ok || !canStarted) ? 1u : 0u,
            .parameters_defaulted = parameters_defaulted() ? 1u : 0u,
            .bus_off_latched = bus.latched ? 1u : 0u,
            .error_passive = bus.error_passive ? 1u : 0u,
            .receive_overflow = bus.rx_overflowed ? 1u : 0u,
        };
        uint8_t frame[CAN_CONTROL_VCU_STATUS_LENGTH];

        vcu_tx_build_status(&status, &statusTx, frame);
        (void)control_bus_send(CAN_CONTROL_VCU_STATUS_FRAME_ID,
                               CAN_CONTROL_VCU_STATUS_IS_EXTENDED != 0, frame,
                               (uint8_t)sizeof(frame));
    }
}

static const char *vcu_state_name(uint8_t state)
{
    switch (state) {
    case CAN_CONTROL_VCU_STATUS_STATE_STARTUP_CHOICE:        return "STARTUP";
    case CAN_CONTROL_VCU_STATUS_STATE_GLV_ON_CHOICE:         return "GLV_ON";
    case CAN_CONTROL_VCU_STATUS_STATE_TS_ACTIVE_CHOICE:      return "TS_ACTIVE";
    case CAN_CONTROL_VCU_STATUS_STATE_READY_TO_DRIVE_CHOICE: return "READY_TO_DRIVE";
    default:                                                 return "?";
    }
}

const char *vcu_step_save_refusal(void)
{
    control_snapshot_t snap;

    control_step_read(&snap);
    if (!snap.running || snap.state != CAN_CONTROL_VCU_STATUS_STATE_GLV_ON_CHOICE) {
        return "only in GLV_ON";
    }
    return NULL;
}

const char *vcu_step_bench_refusal(void)
{
    control_snapshot_t snap;

    control_step_read(&snap);
    if (snap.running && snap.state == CAN_CONTROL_VCU_STATUS_STATE_READY_TO_DRIVE_CHOICE) {
        return "the VCU is in READY_TO_DRIVE";
    }
    return NULL;
}

bool vcu_step_read_inputs(vcu_inputs_t *out)
{
    control_snapshot_t snap;

    control_step_read(&snap);
    if (!snap.running) {
        return false;
    }
    memcpy(out, snap.image_data.bytes, sizeof(*out));
    return true;
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
    .state_name = vcu_state_name,
    .image_data = &inputs,
    .image_data_size = sizeof(inputs),
};
