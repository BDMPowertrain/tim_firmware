/* bms_step.c — the BMS's Control step, see bms_step.h. */

#include "bms_step.h"

#include <stddef.h>
#include <string.h>

#include "bms_charge.h"
#include "bms_control.h"
#include "bms_params.h"
#include "bus_term.h"
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
   Kick (ADR 0013) is never late for the chain. The step with the
   open-wire check runs two, the Sweep's and the check's: their worst
   together is under the period too (host test), with little room to spare,
   so the overrun Event says whether the check ran (spec #36). */
_Static_assert(BQ_PLAN_BUDGET_US < BMS_STEP_PERIOD_MS * 1000u,
               "a cell-monitor plan fits inside one step");

/* The first step starts CAN1 one period after termination was written
   (control_bus_init()), so the period must outlast the settle time. */
_Static_assert(BMS_STEP_PERIOD_MS > BUS_TERM_SETTLE_MS,
               "the first step must find CAN1's termination settled");

/* The BMS makes no decision from a received frame except the charger's
   presence (spec, user story 26), so this is its only mailbox: one exact-
   match extended filter. It is row CHARGER_ROW. */
static const control_bus_rx_t bmsRx[] = {
    { CONTROL_RX_CHG_STATUS, CONTROL_BUS_FIFO0 },
};
#define CHARGER_ROW 0u

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
/* The open-wire check run at the end of the last step, for this one. */
static bool openWireRan;
static bq_open_wire_t openWire;
static uint32_t openWireChecks;
static uint32_t openWireFails;
/* The balancing command run at the end of the last step, for this one. */
static bool balanceRan;
static bq_balance_result_t balanceResult;

/* Read by the shell. */
static bms_pack_t published;

bool bms_step_init(void)
{
    /* Fixed until the next reset (ADR 0015). Static: this runs on
       initTask's small stack (tasks.c). The Held fault (ADR 0007, 0011):
       BMS_OK stays low until the next Power-on reset after any reset that
       was not one, unless a debugger had set the debug enable bit. */
    static bms_control_config_t config;
    const reset_report_t *reset = reset_info_report();

    config = (bms_control_config_t){
        .held = bms_control_held(reset->cause, reset_info_debugger()),
        .params_defaulted = parameters_defaulted(),
        .not_dual_bank = flash_bank2_get() == NULL,
        .last_reset_not_power_on = !reset_cause_is_power_on(reset->cause),
        /* Derating (ADR 0018): each bounded by bms_params.c. */
        .derate = {
            .v_full_mv = (uint16_t)parameters_value(BMS_PARAM_DISCHARGE_V_FULL),
            .v_floor_mv = (uint16_t)parameters_value(BMS_PARAM_DISCHARGE_V_FLOOR),
            .t_full_dc = (int16_t)parameters_value(BMS_PARAM_DISCHARGE_T_FULL),
            .t_floor_dc = (int16_t)parameters_value(BMS_PARAM_DISCHARGE_T_FLOOR),
            .cell_res_uohm = (uint16_t)parameters_value(BMS_PARAM_CELL_RESISTANCE),
            .full_limit_da = (uint16_t)parameters_value(BMS_PARAM_FULL_DISCHARGE_LIMIT),
        },
        /* Charge control (ADR 0019): each bounded by bms_params.c. */
        .charge = {
            .target_mv = (uint16_t)parameters_value(BMS_PARAM_CHARGE_TARGET),
            .full_limit_da = (uint16_t)parameters_value(BMS_PARAM_FULL_CHARGE_LIMIT),
            .v_band_mv = (uint16_t)parameters_value(BMS_PARAM_CHARGE_V_BAND),
            .hot_full_dc = (int16_t)parameters_value(BMS_PARAM_CHARGE_T_HOT_FULL),
            .hot_zero_dc = (int16_t)parameters_value(BMS_PARAM_CHARGE_T_HOT_ZERO),
            .cold_full_dc = (int16_t)parameters_value(BMS_PARAM_CHARGE_T_COLD_FULL),
            .cold_zero_dc = (int16_t)parameters_value(BMS_PARAM_CHARGE_T_COLD_ZERO),
        },
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
        if (pack.sweep.devices_read != BQ_SWEEP_ALL_DEVICES) {
            sweepsMissing++;
        }
    }
    bms_adc_read(&pack.adc);
}

/* Pack current: the one input that gives it (issue 66). Pack current
   sensing is #29, still open, so it is invalid: the voltage taper runs on
   the raw voltage and BMS_Power carries 0 A. #29 replaces this function's
   body and nothing else. */
static bms_current_t read_pack_current(void)
{
    return (bms_current_t){ .deci_a = 0, .valid = false };
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
       needs no mailbox to clear, since the charger is absent in the car. The
       charger's mailbox is updated here, and charge control reads it. */
    control_bus_update(ctx->now_ms, true);

    const control_bus_health_t bus = control_bus_health();

    in.now_ms = ctx->now_ms;
    in.step_count = ctx->step_count;
    in.startup_ok = ctx->startup_ok;
    in.can_running = canStarted;
    in.swept = swept;
    in.sweep = &pack.sweep;
    /* Charge mode and the charger (bms_charge.h); the CHG_MODE pin is not
       read (ADR 0009). */
    in.charger = control_bus_mailbox(CHARGER_ROW);
    /* Active low, like the VCU's SDC_MON_N (issue 19: SDC_OK_N taps the
       end of the shutdown loop). The feedback inputs are read once per
       step, with no debounce (issue 19): FB.NFAULT follows the bridge's
       open-drain NFAULT (high: no fault), FB.BMS_OK the AND of MCU_BMS_OK
       and NFAULT (ADR 0008). */
    in.sdc_closed = HAL_GPIO_ReadPin(SDC_OK_N_GPIO_Port, SDC_OK_N_Pin) == GPIO_PIN_RESET;
    in.nfault_ok = HAL_GPIO_ReadPin(FB_NFAULT_GPIO_Port, FB_NFAULT_Pin) == GPIO_PIN_SET;
    in.bms_ok_fb = HAL_GPIO_ReadPin(FB_BMS_OK_GPIO_Port, FB_BMS_OK_Pin) == GPIO_PIN_SET;
    in.timing_fault = ctx->timing_fault;
    in.open_wire_ran = openWireRan;
    in.open_wire = openWire;
    openWireRan = false;
    in.balance_ran = balanceRan;
    in.balance_result = balanceResult;
    balanceRan = false;
    in.bus_off_latched = bus.latched;
    in.error_passive = bus.error_passive;
    in.rx_overflowed = bus.rx_overflowed;
    in.pack_current = read_pack_current();
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

    /* At most three, one per hardware send slot (bms_tx.h). */
    for (size_t i = 0; i < out.frame_count; i++) {
        const bms_tx_frame_t *f = &out.frames[i];
        (void)control_bus_send(f->id, f->extended, f->data, f->len);
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
    pack.under_low_voltage_limit = out.under_low_voltage_limit;
    pack.discharge_limit_da = out.discharge_limit_da;
    pack.charge_limit_da = out.charge_limit_da;
    pack.charge = out.charge;
    pack.charger_heard = in.charger->state == MAILBOX_VALID;
    pack.pack_current = in.pack_current;
    pack.summary = out.summary;
    pack.nfault_ok = in.nfault_ok;
    pack.bms_ok_fb = in.bms_ok_fb;
    pack.sdc_closed = in.sdc_closed;
    pack.open_wire_checks = openWireChecks;
    pack.open_wire_fails = openWireFails;
    pack.open_wire = openWire;
    pack.balance_state = out.balance_state;
    pack.balance_reason = out.balance_reason;
    pack.cell_spread_mv = out.cell_spread_mv;
    memcpy(pack.bleeding, out.bleeding, sizeof pack.bleeding);
    masked_copy(&published, &pack, sizeof published);
}

/* Balancing's command (bq_chain.h, bms_balance.h), when the logic gives
   one: never on the step with the open-wire check (bms_control.h). A
   command the chain cannot take (not running) is reported as not run. */
static void run_balance_command(control_step_ctx_t *ctx)
{
    cell_monitor_result_t link;

    balanceRan = false;
    if (!bq_chain_plan_balance(&chain, &out.balance_command, &plan)) {
        return;
    }
    ctx->extra_work_ran = true;
    cell_monitor_run(&plan, &link);
    bq_chain_balance_done(&out.balance_command, link.ok, link.resp, link.len, &balanceResult);
    balanceRan = true;
}

/* The open-wire check (bq_chain.h), or balancing's command, when the logic
   asks: after the step's Sweep, judgement, BMS_OK, frames and snapshot, so
   neither delays or replaces any of them (spec #36). The sinks are off
   again before the next Sweep. The result goes to the logic on the next
   step, and to the snapshot then. The overrun Event's flag says that one
   of the two ran. */
static void phase_chain(control_step_ctx_t *ctx)
{
    cell_monitor_result_t link;

    if (out.balance_command.op != BQ_BALANCE_NONE) {
        run_balance_command(ctx);
        return;
    }
    if (!out.open_wire_due || !bq_chain_plan_open_wire(&chain, &plan)) {
        return;
    }
    ctx->extra_work_ran = true;
    cell_monitor_run(&plan, &link);
    bq_chain_open_wire_done(&chain, link.ok, link.resp, link.len, &openWire);
    openWireRan = true;
    openWireChecks++;
    if (openWire.verdict == BQ_OPEN_WIRE_FAIL) {
        openWireFails++;
    }
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
    { "chain",  phase_chain },
};

/* The runner runs no phase past STEP_PHASE_MAX. */
_Static_assert(sizeof(bmsPhases) / sizeof(bmsPhases[0]) <= STEP_PHASE_MAX,
               "every BMS phase runs");

const control_step_def_t bms_step_def = {
    .image = "BMS",
    .period_ms = BMS_STEP_PERIOD_MS,
    .phases = bmsPhases,
    .phase_count = sizeof(bmsPhases) / sizeof(bmsPhases[0]),
    .state_name = bms_control_state_name,
};
