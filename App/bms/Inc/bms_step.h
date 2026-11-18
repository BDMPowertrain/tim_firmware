#ifndef APP_BMS_INC_BMS_STEP_H
#define APP_BMS_INC_BMS_STEP_H

/* The BMS's Control step (ADR 0010): every 20 ms, take one Sweep of the
   cell monitors (sleeping during the link exchange), snapshot the
   charger's mailbox, run the logic, write BMS_OK, send, publish. Before the
   chain is awake the Sweep step wakes it instead (bq_chain.h). No fault
   is judged yet (#63), so the BMS stays in STARTUP with BMS_OK low and
   sends BMS_Status every step. */

#include <stdbool.h>
#include <stdint.h>

#include "bms_adc.h"
#include "bq_chain.h"
#include "bq_sweep.h"
#include "control_step.h"

/* What the step publishes about the pack each step, for the shell and
   later the Data log and the DAQ bus. Copied with interrupts masked (ADR
   0010: the BMS published snapshot, about 550 bytes). */
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
