/* stats.c — on-demand task instrumentation. Everything here reads live
   scheduler state through FreeRTOS's own trace-facility API; nothing is
   cached or computed periodically. CPU % per task is deliberately not
   included yet — that needs configGENERATE_RUN_TIME_STATS plus a
   free-running hardware timer for run-time accounting, neither of which
   is wired up. See Documentation/tim-rtos-architecture.md section 6,
   ladder step 4. */

#include "stats.h"

#include "FreeRTOS.h"
#include "task.h"

#include "print.h"

/* Covers every task this image currently creates (idle, timer service,
   printTask, shellTask, heartbeatTask, blinkTask) with headroom for
   what ladder steps 5 onward add. uxTaskGetSystemState() just truncates
   silently if the real count is ever higher — bump this if `tasks`
   output looks short. */
#define STATS_MAX_TASKS 16u

static const char *stats_state_name(eTaskState state)
{
    switch (state) {
    case eRunning:   return "running";
    case eReady:     return "ready";
    case eBlocked:   return "blocked";
    case eSuspended: return "suspended";
    case eDeleted:   return "deleted";
    default:         return "unknown";
    }
}

void stats_print_tasks(void)
{
    TaskStatus_t status[STATS_MAX_TASKS];
    UBaseType_t count = uxTaskGetSystemState(status, STATS_MAX_TASKS, NULL);

    shell_printf("%-16s %-10s %4s %10s\r\n", "name", "state", "prio", "stack free");

    for (UBaseType_t i = 0; i < count; i++) {
        /* usStackHighWaterMark is in words (FreeRTOS's own unit for it);
           *4 to report bytes, matching the *_STACK_SIZE convention in
           tasks.h so the two numbers are directly comparable. */
        shell_printf("%-16s %-10s %4u %8lu B\r\n",
                     status[i].pcTaskName,
                     stats_state_name(status[i].eCurrentState),
                     (unsigned)status[i].uxCurrentPriority,
                     (unsigned long)status[i].usStackHighWaterMark * sizeof(StackType_t));
    }
}