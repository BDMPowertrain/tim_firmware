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
       osPriorityNormal        24   Default. Shell, CAN TX, general control.
       osPriorityAboveNormal   32   High-rate sensing — IMU, APPS.
       osPriorityHigh          40   Hard hardware deadlines — CAN RX FIFO.
       osPriorityRealtime      48   Torque path / safety interlocks.
       osPriorityISR           56   Reserved for the RTOS's own ISR-deferral thread.

   osPriorityRealtime and osPriorityHigh are RESERVED for the future VCU
   torque path and hard hardware deadlines (CAN RX FIFO). Nothing below
   may use them — see Documentation/tim-rtos-architecture.md section 2
   for the full reserved-band table and why re-tuning later is expensive.

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

#define HEARTBEAT_TASK_PRIORITY   osPriorityLow
#define HEARTBEAT_TASK_STACK_SIZE 256u

#define BLINK_TASK_PRIORITY       osPriorityNormal
#define BLINK_TASK_STACK_SIZE     256u

#define PRINT_TASK_PRIORITY       osPriorityNormal
#define PRINT_TASK_STACK_SIZE     512u

#define SHELL_TASK_PRIORITY       osPriorityNormal
/* Measured via the `tasks` command itself: running `tasks` (builds a
   16-entry TaskStatus_t array on shellTask's own stack, on top of its
   line buffer and shell_dispatch()'s argv array) drove the high-water
   mark down to ~688 B free out of 2048 — i.e. ~1360 B actually used at
   the worst point seen so far. 2048 leaves real headroom; re-measure
   with `tasks` if more call depth gets added to this path later. */
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

/* Creates every queue and task the running image needs, then returns.
   Called once from initTask, which terminates immediately afterward —
   see Documentation/tim-rtos-architecture.md section 3. */
void tasks_init(void);

#endif /* APP_INC_TASKS_H */
