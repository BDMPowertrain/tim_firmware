/* stats.c — see stats.h. Housekeeping writes the table and the shell reads
   it, one row at a time under statsMutex, so printing (which can wait on
   the print queue) never holds the lock. */

#include "stats.h"

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cmsis_os2.h"

#include "print.h"

/* Covers every task either image creates (idle, timer service, print,
   shell, housekeeping, the IMU and logger on the VCU, the two DAQ bus
   tasks, the control step) with headroom. With more tasks than this,
   uxTaskGetSystemState() returns none at all and `tasks` says "none
   gathered yet" for good: bump this if it does. */
#define STATS_MAX_TASKS 16u

typedef struct {
    char        name[configMAX_TASK_NAME_LEN];
    eTaskState  state;
    UBaseType_t priority;
    uint32_t    stack_free_bytes;
    uint32_t    cpu_permille; /* of the run time since the last gathering */
} stats_row_t;

/* Housekeeping's alone: the last gathering's run-time counters, to take
   each task's run time in this window by difference. */
typedef struct {
    UBaseType_t number; /* FreeRTOS's unique task number */
    uint32_t    run_time;
} stats_prev_t;

static TaskStatus_t status[STATS_MAX_TASKS];
static stats_prev_t prev[STATS_MAX_TASKS];
static UBaseType_t  prevCount;
static uint32_t     windowRun[STATS_MAX_TASKS];

/* Shared with the shell, under statsMutex. */
static osMutexId_t statsMutex;
static stats_row_t rows[STATS_MAX_TASKS];
static UBaseType_t rowCount;
static uint32_t    gatherings;

bool stats_init(void)
{
    const osMutexAttr_t attr = { .name = "stats" };
    statsMutex = osMutexNew(&attr);
    return statsMutex != NULL;
}

/* The run time in this window: the counter's rise since the last
   gathering, by unsigned difference (each counter wraps every 25 s of its
   own task's time, far beyond one second). A task new since then is
   counted from its creation. */
static uint32_t window_run_time(const TaskStatus_t *task)
{
    for (UBaseType_t i = 0; i < prevCount; i++) {
        if (prev[i].number == task->xTaskNumber) {
            return task->ulRunTimeCounter - prev[i].run_time;
        }
    }
    return task->ulRunTimeCounter;
}

void stats_gather(void)
{
    /* uxTaskGetSystemState() suspends the scheduler while it walks every
       task and scans each one's unused stack for the high-water mark, so
       the Control step cannot start until it ends (an estimated few hundred
       microseconds; unmeasured, watch the step's worst time with `step`).
       Housekeeping's boundaries fall on the step's, and the step, being
       higher, runs first: so this normally runs just after a step, with
       nearly a whole period to the next. */
    const UBaseType_t count = uxTaskGetSystemState(status, STATS_MAX_TASKS, NULL);

    /* The window's total is the sum of every task's part, the Idle task
       included, so the shares add to 1000 and need no separate clock. */
    uint64_t total = 0u;
    for (UBaseType_t i = 0; i < count; i++) {
        windowRun[i] = window_run_time(&status[i]);
        total += windowRun[i];
    }

    if (statsMutex == NULL || osMutexAcquire(statsMutex, osWaitForever) != osOK) {
        return;
    }
    for (UBaseType_t i = 0; i < count; i++) {
        stats_row_t *row = &rows[i];
        strncpy(row->name, status[i].pcTaskName, sizeof(row->name) - 1u);
        row->name[sizeof(row->name) - 1u] = '\0';
        row->state = status[i].eCurrentState;
        row->priority = status[i].uxCurrentPriority;
        /* usStackHighWaterMark is in words; bytes, to compare directly
           with the *_STACK_SIZE values in tasks.h. */
        row->stack_free_bytes = (uint32_t)status[i].usStackHighWaterMark * sizeof(StackType_t);
        row->cpu_permille = (total == 0u)
            ? 0u
            : (uint32_t)(((uint64_t)windowRun[i] * 1000u + total / 2u) / total);
    }
    rowCount = count;
    gatherings++;
    (void)osMutexRelease(statsMutex);

    for (UBaseType_t i = 0; i < count; i++) {
        prev[i].number = status[i].xTaskNumber;
        prev[i].run_time = status[i].ulRunTimeCounter;
    }
    prevCount = count;
}

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

/* One row, or false past the end. */
static bool read_row(UBaseType_t index, stats_row_t *out, uint32_t *gathering)
{
    bool have = false;

    if (statsMutex == NULL || osMutexAcquire(statsMutex, osWaitForever) != osOK) {
        return false;
    }
    if (index < rowCount) {
        *out = rows[index];
        *gathering = gatherings;
        have = true;
    }
    (void)osMutexRelease(statsMutex);
    return have;
}

void stats_print_tasks(void)
{
    stats_row_t row;
    uint32_t first = 0u;
    uint32_t gathering = 0u;

    if (!read_row(0u, &row, &first)) {
        shell_printf("tasks: none gathered yet (once a second)\r\n");
        return;
    }

    shell_printf("%-16s %-10s %4s %6s %10s\r\n", "name", "state", "prio", "cpu", "stack free");
    for (UBaseType_t i = 0u; read_row(i, &row, &gathering); i++) {
        shell_printf("%-16s %-10s %4u %3lu.%lu%% %8lu B\r\n",
                     row.name, stats_state_name(row.state), (unsigned)row.priority,
                     (unsigned long)(row.cpu_permille / 10u),
                     (unsigned long)(row.cpu_permille % 10u),
                     (unsigned long)row.stack_free_bytes);
    }
    if (gathering != first) {
        shell_printf("(rows span two gatherings)\r\n");
    }
}
