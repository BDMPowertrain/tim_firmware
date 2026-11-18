/* bms_step.c — the BMS's Control step, see bms_step.h. */

#include "bms_step.h"

#include <stddef.h>
#include <string.h>

#include "bms_control.h"
#include "bms_params.h"
#include "bus_term.h"
#include "can_control.h"
#include "cell_monitor.h"
#include "control_bus.h"
#include "events.h"
#include "flash_bank2.h"
#include "main.h"
#include "masked_copy.h"
#include "parameters.h"
#include "reset_cause.h"
#include "reset_info.h"

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

/* Owned by the step; nothing else touches them. Static, not on the step's
   stack: the output holds the Events. */
static bool canStarted;
static bq_chain_t chain;
static bms_control_t control;
static bms_control_in_t in;
static bms_control_out_t out;
static bq_plan_t plan;   /* static: about 600 bytes, off the step's stack */
static bms_pack_t pack;  /* this step's, published at its end */
static uint32_t sweeps;
static uint32_t sweepsMissing;
static bool swept; /* this step took a Sweep */

/* Read by the shell. */
static bms_pack_t published;

bool bms_step_init(void)
{
    /* Fixed until the next reset (ADR 0015). Static: this runs on
       initTask's small stack (tasks.c). The Held fault is decided by #65:
       until then no reset holds BMS_OK low. */
    static bms_control_config_t config;

    config = (bms_control_config_t){
        .held = false,
        .params_defaulted = parameters_defaulted(),
        .not_dual_bank = flash_bank2_get() == NULL,
        .last_reset_not_power_on = !reset_cause_is_power_on(reset_info_report()->cause),
    };
    bms_control_init(&control, &config);
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
    swept = bq_chain_done(&chain, ctx->now_ms, link.ok, link.resp, link.len, &pack.sweep);
    if (swept) {
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

    const control_bus_health_t bus = control_bus_health();

    in.now_ms = ctx->now_ms;
    in.step_count = ctx->step_count;
    in.startup_ok = ctx->startup_ok;
    in.can_running = canStarted;
    in.swept = swept;
    in.sweep = &pack.sweep;
    in.charge_mode = false; /* #67 */
    /* Active low, like the VCU's SDC_MON_N (issue 19: SDC_OK_N taps the
       end of the shutdown loop). A status bit only until #64 checks it
       against BMS_OK. */
    in.sdc_closed = HAL_GPIO_ReadPin(SDC_OK_N_GPIO_Port, SDC_OK_N_Pin) == GPIO_PIN_RESET;
    in.bus_off_latched = bus.latched;
    in.error_passive = bus.error_passive;
    in.rx_overflowed = bus.rx_overflowed;
}

/* Judge (bms_control.h): the state, BMS_OK and BMS_Status from this
   step's Sweep. */
static void phase_logic(control_step_ctx_t *ctx)
{
    bms_control_step(&control, &in, &out);
    ctx->state = out.state;
    ctx->fault_active = out.fault_active;
}

/* Write BMS_OK, send, publish, in that order (spec #36). */
static void phase_output(control_step_ctx_t *ctx)
{
    /* Written every step, as the step is its only writer; high = no fault. */
    HAL_GPIO_WritePin(MCU_BMS_OK_GPIO_Port, MCU_BMS_OK_Pin,
                      out.bms_ok ? GPIO_PIN_SET : GPIO_PIN_RESET);

    if (out.send_status) {
        (void)control_bus_send(CAN_CONTROL_BMS_STATUS_FRAME_ID,
                               CAN_CONTROL_BMS_STATUS_IS_EXTENDED != 0, out.status,
                               (uint8_t)sizeof(out.status));
    }
    /* The Event queue never waits; a full one drops and counts (events.h). */
    for (size_t i = 0; i < out.event_count; i++) {
        (void)events_post(out.events[i].code, out.events[i].data);
    }

    pack.step_count = ctx->step_count;
    pack.chain_state = bq_chain_state(&chain);
    pack.wake_attempts = bq_chain_attempts(&chain);
    pack.sweeps = sweeps;
    pack.sweeps_missing = sweepsMissing;
    pack.link_timeouts = cell_monitor_timeouts();
    pack.faults_active = out.active;
    pack.faults_seen = out.seen;
    pack.low_voltage_limit = out.low_voltage_limit;
    masked_copy(&published, &pack, sizeof published);
}

void bms_step_read_pack(bms_pack_t *dst)
{
    masked_copy(dst, &published, sizeof *dst);
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
    .state_name = bms_control_state_name,
};
