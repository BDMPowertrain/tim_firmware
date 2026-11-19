#ifndef APP_BMS_INC_BMS_STEP_H
#define APP_BMS_INC_BMS_STEP_H

/* The BMS's Control step (ADR 0010): every 20 ms, take one Sweep of the
   cell monitors (sleeping during the link exchange), snapshot the
   charger's mailbox, run the logic, write BMS_OK, send, publish. Before the
   chain is awake the Sweep step wakes it instead (bq_chain.h). The BMS
   faults, the states, BMS_OK and the Discharge limit are judged by
   bms_control.h; the frames go as bms_tx.h's send table says. When the
   logic asks, once a second, the open-wire check runs last in the step,
   in a phase of its own (bq_chain.h); in Charge mode balancing's chain
   commands run there too, never in the same step as the check
   (bms_balance.h). */

#include <stdbool.h>
#include <stdint.h>

#include "bms_adc.h"
#include "bms_control.h"
#include "bq_chain.h"
#include "bq_sweep.h"
#include "control_step.h"

/* What the step publishes about the pack each step, for the shell and
   later the Data log and the DAQ bus. Copied with interrupts masked (ADR
   0010: the BMS published snapshot; about 550 bytes, a little over that
   ADR's 500-byte estimate). */
typedef struct {
    uint32_t         step_count;     /* the step that published it */
    bq_chain_state_t chain_state;
    uint32_t         wake_attempts;  /* wake pings since power-on */
    uint32_t         sweeps;         /* Sweeps taken since power-on */
    uint32_t         sweeps_missing; /* of them, with a device not read */
    uint32_t         link_timeouts;  /* reads that timed out since power-on */
    /* This step's Sweep. Every device is missing (devices_read 0) on a
       step that took none: the chain was waking. */
    bq_sweep_t       sweep;
    bms_adc_counts_t adc;            /* the newest analog counts */
    uint16_t         faults_active;  /* bit (1 << bms_fault_t), bms_control.h */
    uint16_t         faults_seen;    /* since power-on */
    bool             under_low_voltage_limit;
    uint16_t         discharge_limit_da; /* the Discharge limit sent, 0.1 A */
    uint16_t         charge_limit_da;    /* the Charge limit sent, 0.1 A */
    bms_charge_state_t charge;           /* issue 67 */
    bool             charger_heard;      /* its mailbox VALID */
    bms_current_t    pack_current;       /* as the step took it */
    bms_pack_summary_t summary;          /* the last Sweep in brief */
    /* The second layers (issue 64), as this step read them. */
    bool             nfault_ok;      /* FB.NFAULT high */
    bool             bms_ok_fb;      /* FB.BMS_OK high */
    bool             sdc_closed;     /* the Shutdown Circuit sense energised */
    uint32_t         open_wire_checks; /* run since power-on */
    uint32_t         open_wire_fails;  /* of them, failed */
    bq_open_wire_t   open_wire;      /* the last one's result */
    /* Balancing (issue 68, bms_balance.h). */
    uint8_t          balance_state;  /* BMS_Balancing State */
    uint8_t          balance_reason;
    uint16_t         cell_spread_mv; /* the last settled Sweep's */
    /* Bit k of bleeding[d - 1]: cell k + 1 of device d commanded on, the
       DAQ bus's per-cell bleed flags. */
    uint8_t          bleeding[BQ_SWEEP_DEVICES];
} bms_pack_t;

/* Sets up the Control bus for the one frame the BMS reads, the charger's
   status. Call from tasks_init() before the step is created. False if
   CAN1 refused. */
bool bms_step_init(void);

/* Copies the latest published pack snapshot. Any task. */
void bms_step_read_pack(bms_pack_t *out);

/* The step itself, for control_step_task(). */
extern const control_step_def_t bms_step_def;

#endif /* APP_BMS_INC_BMS_STEP_H */
