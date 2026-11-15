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

/* Creates every queue and task the BMS image needs, then returns. Called
   once from initTask, which terminates immediately afterward. */
void tasks_init(void);

#endif /* APP_BMS_INC_TASKS_H */
