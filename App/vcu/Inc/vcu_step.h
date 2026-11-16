#ifndef APP_VCU_INC_VCU_STEP_H
#define APP_VCU_INC_VCU_STEP_H

/* The VCU's Control step (ADR 0010): every 10 ms, snapshot the mailboxes,
   run the logic, send. The logic is still empty, so the VCU stays in
   STARTUP and sends VCU_Status every second step. */

#include <stdbool.h>

#include "control_step.h"

/* Sets up the Control bus for the frames the VCU reads. Call from
   tasks_init() before the step is created. False if CAN1 refused. */
bool vcu_step_init(void);

/* The step itself, for control_step_task(). */
extern const control_step_def_t vcu_step_def;

#endif /* APP_VCU_INC_VCU_STEP_H */
