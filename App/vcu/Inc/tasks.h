#ifndef APP_INC_TASKS_H
#define APP_INC_TASKS_H

#include "cmsis_os2.h"

/* Every task priority and stack size lives here, per CLAUDE.md — one place
   to look when tuning, and CubeMX regeneration never touches this file.

   ---------------------------------------------------------------------
   osPriority_t — how the scheduler picks who runs next
   ---------------------------------------------------------------------
   FreeRTOS is a preemptive, priority-based scheduler: at every tick it
   runs the highest-priority task that is currently ready, full stop. A
   ready lower-priority task never gets the CPU while a higher-priority
   one is ready — it doesn't matter how long the lower one has been
   waiting. A task stops being "ready" while it's blocked — osDelay(),
   waiting on a queue/mutex/semaphore/stream buffer — which is what lets
   lower-priority tasks run at all. Two tasks at the *same* priority
   round-robin, each getting one tick slice in turn (configTICK_RATE_HZ
   here is 1000, i.e. a 1 ms tick, so osDelay(5) really means 5 ms).

   CMSIS-RTOS v2 (the wrapper this project uses instead of calling
   FreeRTOS's native xTaskCreate/etc. directly) expresses priority as the
   enum osPriority_t, low value = low priority. The named levels, in
   ascending order, each with 8 sub-steps (osPriorityLow1..Low7, etc.,
   value+1..value+7) for finer-grained ranking if a project ever needs
   it — this project only ever uses the base name of a band:

       osPriorityIdle           1   FreeRTOS's own Idle task only. Never assign.
       osPriorityLow            8   Housekeeping, heartbeat LED.
       osPriorityBelowNormal   16   Logging, slow sensing, stats.
       osPriorityNormal        24   Default. Shell, DAQ bus (CAN2) tasks.
       osPriorityAboveNormal   32   High-rate sensing — IMU, APPS.
       osPriorityHigh          40   Empty (ADR 0013).
       osPriorityRealtime      48   The control step, and nothing else.
       osPriorityISR           56   Reserved for the RTOS's own ISR-deferral thread.

   osPriorityRealtime is the control step's alone (ADR 0010) and
   osPriorityHigh stays empty (ADR 0013). Nothing else may use them — see
   Documentation/tim-rtos-architecture.md section 2 for why re-tuning
   later is expensive.

   ---------------------------------------------------------------------
   stack_size — what a task's stack actually is
   ---------------------------------------------------------------------
   Each task gets its own private stack, carved out of the FreeRTOS heap
   (configTOTAL_HEAP_SIZE, 32 KB on this board) when osThreadNew() runs.
   It holds that task's local variables, call frames, and saved registers
   across every function it calls — completely separate from every other
   task's stack, which is what lets tasks safely call the same functions
   (shell_printf(), HAL_ calls, etc.) at once without stepping on each
   other's data.

   osThreadAttr_t.stack_size, and the *_STACK_SIZE defines below, are in
   BYTES (cmsis_os2's osThreadNew() divides by sizeof(StackType_t) == 4
   internally before handing it to FreeRTOS, which counts stack in
   32-bit words on this Cortex-M4 port). configMINIMAL_STACK_SIZE (128
   words = 512 bytes) is the floor FreeRTOS itself uses for the Idle
   task — treat that as the minimum for anything that isn't trivial.

   Undersizing a stack does NOT fail cleanly. The task's stack pointer
   just walks past the end of its allocation into whatever memory is
   next, corrupting another task's stack or RTOS state, and the crash
   shows up somewhere unrelated, often much later. This is exactly why
   the doc's ladder step 4 (statsTask + uxTaskGetStackHighWaterMark())
   and configCHECK_FOR_STACK_OVERFLOW exist — don't guess a stack size
   and walk away from it; measure the high-water mark once the task is
   exercised and size to that with headroom. */

/* Housekeeping (issue 47): the one Low task, every 100 ms — heartbeat LED,
   VBAT, and once a second the task statistics (whose array is static in
   stats.c, not on this stack). The stack holds a copy of the step's
   snapshot and VBAT's ADC window average. Measured on the bench TIM
   (Debug, 20 s, issue 55): 608 B free of 1024. */
#define HOUSEKEEPING_TASK_PRIORITY   osPriorityLow
#define HOUSEKEEPING_TASK_STACK_SIZE 1024u

#define PRINT_TASK_PRIORITY       osPriorityNormal
#define PRINT_TASK_STACK_SIZE     512u

#define SHELL_TASK_PRIORITY       osPriorityNormal
/* Measured via the `tasks` command itself when it still built a 16-entry
   TaskStatus_t array on shellTask's own stack, on top of its line buffer
   and shell_dispatch()'s argv array: ~688 B free out of 2048. `tasks` now
   copies one gathered row at a time (stats.c), so this has more headroom
   than that; re-measure with `tasks` if more call depth gets added. */
#define SHELL_TASK_STACK_SIZE     2048u

/* High-rate sensing band, per the reserved-priority table in
   Documentation/tim-rtos-architecture.md section 2. Measured via
   `tasks` with the driver actually parsing live FIFO bursts (not just
   idling): high-water mark settled at 400 B free out of 1024 — real
   headroom, unlike SHELL_TASK_STACK_SIZE's original guess. No change
   needed; imu_drain_fifo()'s big buffers are static, not stack-local,
   which is why this stayed comfortable. */
#define IMU_TASK_PRIORITY         osPriorityAboveNormal
#define IMU_TASK_STACK_SIZE       1024u

/* "Logging, slow sensing, stats" band per the reserved-priority table.
   FatFS's f_mount/f_open/f_write call chain (ff.c) is known to be
   stack-hungry even with long filenames disabled (_USE_LFN=0 in
   ffconf.h) — starting at the same size that turned out right for
   shellTask rather than guessing low; measure with `tasks` once
   `log start` has actually written a file and adjust. */
#define LOGGER_TASK_PRIORITY      osPriorityBelowNormal
#define LOGGER_TASK_STACK_SIZE    2048u

/* The DAQ bus (CAN2) receive task and send task (daqTask, which also runs
   the 100 ms DAQ publisher). The DAQ bus carries no safety function, so
   both sit in the Normal band with the shell (issue 36's task table); the
   3-deep hardware FIFO is drained fully per interrupt into an 8-deep
   queue, so a late canRxTask drops only logging data. The Control bus
   (CAN1) has no task: its interrupt writes the mailboxes and the control
   step sends (ADR 0004). daqTask's stack holds a snapshot copy and two
   frames; unmeasured. Measure with `tasks` and adjust. */
#define CAN_RX_TASK_PRIORITY      osPriorityNormal
#define CAN_RX_TASK_STACK_SIZE    512u

#define DAQ_TASK_PRIORITY         osPriorityNormal
#define DAQ_TASK_STACK_SIZE       1024u

/* The Event writer (issue 51): BelowNormal, as the Event log only records.
   It opens the log (scanning 32 pages, repairing any torn one through the
   log's static page buffer, not this stack), then takes the Event queue.
   The stack holds one queued Event, the flood guard and the flash calls;
   unmeasured. Measure with `tasks` and adjust. */
#define EVENT_TASK_PRIORITY       osPriorityBelowNormal
#define EVENT_TASK_STACK_SIZE     1024u

/* The Control step (ADR 0010): the one Realtime task, every 10 ms. It
   takes no mutex and blocks only on its own period. The stack holds the
   status frame build, the published snapshot it builds, the ADC window
   averages and the step's own locals; mailboxes and their snapshot are
   static in control_bus.c. Measured on the bench TIM (Debug, 20 s, issue
   55): 432 B free of 1024. */
#define CONTROL_STEP_TASK_PRIORITY   osPriorityRealtime
#define CONTROL_STEP_TASK_STACK_SIZE 1024u

/* Creates every queue and task the running image needs, the control step
   last, then returns. A failed creation does not stop the board: the
   control step still runs and the VCU stays in STARTUP. Called once from
   initTask, which terminates immediately afterward — see
   Documentation/tim-rtos-architecture.md section 3. */
void tasks_init(void);

#endif /* APP_INC_TASKS_H */
