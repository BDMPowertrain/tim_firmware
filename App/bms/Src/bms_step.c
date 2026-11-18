/* bms_step.c — the BMS's Control step, see bms_step.h. */

#include "bms_step.h"

#include <stddef.h>
#include <string.h>

#include "bms_params.h"
#include "bms_tx.h"
#include "bus_term.h"
#include "can_control.h"
#include "cell_monitor.h"
#include "control_bus.h"
#include "e2e_p11.h"
#include "main.h"
#include "masked_copy.h"
#include "parameters.h"

#define BMS_STEP_PERIOD_MS 20u

/* Every plan fits in the step with room for the rest of it, so the next
   Kick (ADR 0013) is never late for the chain. */
_Static_assert(BQ_PLAN_BUDGET_US < BMS_STEP_PERIOD_MS * 1000u,
               "a cell-monitor plan fits inside one step");

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
static bq_chain_t chain;
static bq_plan_t plan;   /* static: about 600 bytes, off the step's stack */
static bms_pack_t pack;  /* this step's, published at its end */
static uint32_t sweeps;
static uint32_t sweepsMissing;

/* Read by the shell. */
static bms_pack_t published;

bool bms_step_init(void)
{
    bq_chain_init(&chain);
    return control_bus_init(bmsRx, sizeof(bmsRx) / sizeof(bmsRx[0]),
                            parameters_value(BMS_PARAM_CAN1_TERM) != 0);
}

/* One Sweep, or the next part of the chain's wake: the step sleeps during
   the link exchange (ADR 0010). A device that did not answer, a timeout
   included, is a missing measurement in the Sweep. */
static void phase_sweep(control_step_ctx_t *ctx)
{
    cell_monitor_result_t link;

    bq_chain_plan(&chain, ctx->now_ms, &plan);
    cell_monitor_run(&plan, &link);

    memset(&pack.sweep, 0, sizeof pack.sweep);
    if (bq_chain_done(&chain, ctx->now_ms, link.ok, link.resp, link.len, &pack.sweep)) {
        sweeps++;
        if (pack.sweep.devices_read != (uint16_t)((1u << BQ_SWEEP_DEVICES) - 1u)) {
            sweepsMissing++;
        }
    }
    bms_adc_read(&pack.adc);
}

static void phase_input(control_step_ctx_t *ctx)
{
    /* CAN1 starts on the first step, a full period after the transceiver
       left standby and termination was written in bms_step_init(). A
       failure is visible as zero frames sent in the shell's `step`, on the
       bench tool and as a 5 Hz heartbeat. */
    if (ctx->step_count == 0u) {
        canStarted = control_bus_start();
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
    /* BMS_OK stays low until the faults and its rise conditions exist
       (#63): written every step, as the step is its only writer. */
    HAL_GPIO_WritePin(MCU_BMS_OK_GPIO_Port, MCU_BMS_OK_Pin, GPIO_PIN_RESET);

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

    /* Publish, last (spec, issue 36: sweep, judge, BMS_OK, send, publish). */
    pack.step_count = ctx->step_count;
    pack.chain_state = bq_chain_state(&chain);
    pack.wake_attempts = bq_chain_attempts(&chain);
    pack.sweeps = sweeps;
    pack.sweeps_missing = sweepsMissing;
    pack.link_timeouts = cell_monitor_timeouts();
    masked_copy(&published, &pack, sizeof published);
}

void bms_step_read_pack(bms_pack_t *out)
{
    masked_copy(out, &published, sizeof *out);
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
    { "sweep",  phase_sweep },
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
