/* bms_step.c — the BMS's Control step, see bms_step.h. */

#include "bms_step.h"

#include <stddef.h>

#include "bms_tx.h"
#include "can_control.h"
#include "control_bus.h"
#include "e2e_p11.h"
#include "parameters.h"

#define BMS_STEP_PERIOD_MS 20u

/* The BMS makes no decision from a received frame except the charger's
   presence (spec, user story 26), so this is its only mailbox: one exact-
   match extended filter. */
static const control_bus_rx_t bmsRx[] = {
    { CONTROL_RX_CHG_STATUS, CONTROL_BUS_FIFO0 },
};

/* Owned by the step; nothing else touches them. */
static e2e_p11_tx_t statusTx;

bool bms_step_init(void)
{
    return control_bus_init(bmsRx, sizeof(bmsRx) / sizeof(bmsRx[0]));
}

static void phase_input(control_step_ctx_t *ctx)
{
    /* CAN1 starts on the first step, a full period after the transceiver
       left standby in bms_step_init(). A failure is visible as zero frames
       sent in the shell's `step` and on the bench tool. */
    if (ctx->step_count == 0u) {
        (void)control_bus_start();
    }
    control_bus_update(ctx->now_ms);
}

static void phase_logic(control_step_ctx_t *ctx)
{
    /* No Sweep or logic yet: the BMS stays in STARTUP. */
    ctx->state = CAN_CONTROL_BMS_STATUS_STATE_STARTUP_CHOICE;
}

static void phase_output(control_step_ctx_t *ctx)
{
    if (bms_tx_status_due(ctx->step_count)) {
        /* BMS_OK is commanded low in STARTUP: every other signal is 0. */
        struct can_control_bms_status_t status = {
            .state = ctx->state,
            .parameters_defaulted = parameters_defaulted() ? 1u : 0u,
        };
        uint8_t frame[CAN_CONTROL_BMS_STATUS_LENGTH];

        bms_tx_build_status(&status, &statusTx, frame);
        (void)control_bus_send(CAN_CONTROL_BMS_STATUS_FRAME_ID,
                               CAN_CONTROL_BMS_STATUS_IS_EXTENDED != 0, frame,
                               (uint8_t)sizeof(frame));
    }
}

static const char *bms_state_name(uint8_t state)
{
    switch (state) {
    case CAN_CONTROL_BMS_STATUS_STATE_STARTUP_CHOICE:    return "STARTUP";
    case CAN_CONTROL_BMS_STATUS_STATE_DRIVE_CHOICE:      return "DRIVE";
    case CAN_CONTROL_BMS_STATUS_STATE_CHARGE_CHOICE:     return "CHARGE";
    case CAN_CONTROL_BMS_STATUS_STATE_FAULT_CHOICE:      return "FAULT";
    case CAN_CONTROL_BMS_STATUS_STATE_FAULT_HELD_CHOICE: return "FAULT_HELD";
    default:                                             return "?";
    }
}

static const control_phase_t bmsPhases[] = {
    { "input",  phase_input },
    { "logic",  phase_logic },
    { "output", phase_output },
};

const control_step_def_t bms_step_def = {
    .image = "BMS",
    .period_ms = BMS_STEP_PERIOD_MS,
    .phases = bmsPhases,
    .phase_count = sizeof(bmsPhases) / sizeof(bmsPhases[0]),
    .state_name = bms_state_name,
};
