/* bms_step.c — the BMS's Control step, see bms_step.h. */

#include "bms_step.h"

#include <stddef.h>

#include "bms_params.h"
#include "bms_tx.h"
#include "bus_term.h"
#include "can_control.h"
#include "control_bus.h"
#include "e2e_p11.h"
#include "parameters.h"

#define BMS_STEP_PERIOD_MS 20u

/* The first step starts CAN1 one period after termination was written
   (control_bus_init()), so the period must outlast the settle time. */
_Static_assert(BMS_STEP_PERIOD_MS > BUS_TERM_SETTLE_MS,
               "the first step must find CAN1's termination settled");

/* The BMS makes no decision from a received frame except the charger's
   presence (spec, user story 26), so this is its only mailbox: one exact-
   match extended filter. */
static const control_bus_rx_t bmsRx[] = {
    { CONTROL_RX_CHG_STATUS, CONTROL_BUS_FIFO0 },
};

/* Owned by the step; nothing else touches them. */
static bool canStarted;
static e2e_p11_tx_t statusTx;

bool bms_step_init(void)
{
    return control_bus_init(bmsRx, sizeof(bmsRx) / sizeof(bmsRx[0]),
                            parameters_value(BMS_PARAM_CAN1_TERM) != 0);
}

static void phase_input(control_step_ctx_t *ctx)
{
    /* CAN1 starts on the first step, a full period after the transceiver
       left standby and termination was written in bms_step_init(). A failure is visible as zero frames
       sent in the shell's `step`, on the bench tool and as a 5 Hz
       heartbeat. */
    if (ctx->step_count == 0u) {
        canStarted = control_bus_start(ctx->now_ms);
    }
    /* Bus-off never stops this step or touches BMS_OK (ADR 0005): sends
       are skipped until the restart and the Sweep carries on. The latch
       needs no mailbox to clear, since the charger is absent in the car. */
    control_bus_update(ctx->now_ms, true);
}

static void phase_logic(control_step_ctx_t *ctx)
{
    /* No Sweep or logic yet: the BMS stays in STARTUP. A blocked start-up
       (a task or queue missing, or CAN1 refused) is the only fault so far:
       the heartbeat shows it at 5 Hz. BMS faults join it with their logic
       (#63, #65). */
    ctx->state = CAN_CONTROL_BMS_STATUS_STATE_STARTUP_CHOICE;
    ctx->fault_active = !ctx->startup_ok || !canStarted;
}

static void phase_output(control_step_ctx_t *ctx)
{
    if (bms_tx_status_due(ctx->step_count)) {
        /* BMS_OK is commanded low in STARTUP: every fault signal is 0. */
        const control_bus_health_t bus = control_bus_health();
        struct can_control_bms_status_t status = {
            .state = ctx->state,
            .parameters_defaulted = parameters_defaulted() ? 1u : 0u,
            .bus_off_latched = bus.latched ? 1u : 0u,
            .error_passive = bus.error_passive ? 1u : 0u,
            .receive_overflow = bus.rx_overflowed ? 1u : 0u,
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
