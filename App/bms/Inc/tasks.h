#ifndef APP_BMS_INC_TASKS_H
#define APP_BMS_INC_TASKS_H

#include "cmsis_os2.h"

/* Every BMS task priority and stack size lives here — one place to look
   when tuning, and CubeMX regeneration never touches this file. The VCU
   image has its own in App/vcu/Inc/tasks.h, which also explains the
   priority bands and why a stack size is measured rather than guessed.

   Stack sizes are in bytes. osPriorityRealtime and osPriorityHigh are
   reserved for the BMS control step and hard hardware deadlines; nothing
   below may use them. */

#define HEARTBEAT_TASK_PRIORITY   osPriorityLow
#define HEARTBEAT_TASK_STACK_SIZE 256u

#define PRINT_TASK_PRIORITY       osPriorityNormal
#define PRINT_TASK_STACK_SIZE     512u

/* Same size as the VCU's shellTask, where `tasks` was measured as the
   deepest command. Re-measure with `tasks` when the BMS gains commands
   of its own. */
#define SHELL_TASK_PRIORITY       osPriorityNormal
#define SHELL_TASK_STACK_SIZE     2048u

/* The DAQ bus (CAN2) receive task and send task (daqTask, with the 100 ms
   DAQ publisher), in the Normal band as on the VCU; the sizes are the
   VCU's (App/vcu/Inc/tasks.h), unmeasured on the BMS. */
#define CAN_RX_TASK_PRIORITY      osPriorityNormal
#define CAN_RX_TASK_STACK_SIZE    512u

#define DAQ_TASK_PRIORITY         osPriorityNormal
#define DAQ_TASK_STACK_SIZE       1024u

/* The Control step (ADR 0010): the one Realtime task, every 20 ms. It
   takes no mutex and blocks only on its own period (and, once the Sweep
   exists, its own cell-monitor exchange). Measure with `tasks`. */
#define CONTROL_STEP_TASK_PRIORITY   osPriorityRealtime
#define CONTROL_STEP_TASK_STACK_SIZE 1024u

/* Creates every queue and task the BMS image needs, the control step last,
   then returns. A failed creation does not stop the board: the control
   step still runs and the BMS stays in STARTUP. Called once from
   initTask, which terminates immediately afterward. */
void tasks_init(void);

#endif /* APP_BMS_INC_TASKS_H */
