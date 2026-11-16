/* vcu_step.c — the VCU's Control step, see vcu_step.h. */

#include "vcu_step.h"

#include <stddef.h>

#include "can_control.h"
#include "control_bus.h"
#include "e2e_p11.h"
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

bool vcu_step_init(void)
{
    return control_bus_init(vcuRx, sizeof(vcuRx) / sizeof(vcuRx[0]));
}

static void phase_input(control_step_ctx_t *ctx)
{
    /* CAN1 starts on the first step, a full period after the transceiver
       left standby in vcu_step_init(). */
    if (ctx->step_count == 0u) {
        canStarted = control_bus_start();
    }
    control_bus_update(ctx->now_ms);
}

static void phase_logic(control_step_ctx_t *ctx)
{
    /* No logic yet: the VCU stays in STARTUP. */
    ctx->state = CAN_CONTROL_VCU_STATUS_STATE_STARTUP_CHOICE;
}

static void phase_output(control_step_ctx_t *ctx)
{
    if (vcu_tx_status_due(ctx->step_count)) {
        struct can_control_vcu_status_t status = {
            .state = ctx->state,
            .startup_blocked = (!ctx->startup_ok || !canStarted) ? 1u : 0u,
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
};
