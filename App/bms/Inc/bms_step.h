#ifndef APP_BMS_INC_BMS_STEP_H
#define APP_BMS_INC_BMS_STEP_H

/* The BMS's Control step (ADR 0010): every 20 ms, snapshot the charger's
   mailbox, run the logic, send. The Sweep and the logic are still empty,
   so the BMS stays in STARTUP with BMS_OK low and sends BMS_Status every
   step. */

#include <stdbool.h>

#include "control_step.h"

/* Sets up the Control bus for the one frame the BMS reads, the charger's
   status. Call from tasks_init() before the step is created. False if
   CAN1 refused. */
bool bms_step_init(void);

/* The step itself, for control_step_task(). */
extern const control_step_def_t bms_step_def;

#endif /* APP_BMS_INC_BMS_STEP_H */
