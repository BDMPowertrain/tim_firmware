/* housekeeping.c — see housekeeping.h. */

#include "housekeeping.h"

#include <stddef.h>

#include "cmsis_os2.h"

#include "control_step.h"
#include "heartbeat.h"
#include "main.h"
#include "masked_copy.h"
#include "stats.h"
#include "step_timing.h"

/* Statistics are gathered every this many runs: once a second. */
#define STATS_EVERY_RUNS (1000u / HOUSEKEEPING_PERIOD_MS)

/* Written only by the task; the step copies it with masked_copy(). */
static uint32_t runs;

bool housekeeping_init(void)
{
    return stats_init();
}

uint32_t housekeeping_count(void)
{
    uint32_t count;
    masked_copy(&count, &runs, sizeof(count));
    return count;
}

void housekeepingTask(void *argument)
{
    const housekeeping_def_t *def = (const housekeeping_def_t *)argument;
    heartbeat_t heartbeat = {0};
    step_sched_t sched;

    /* The step's own scheduler: fixed boundaries, no catch-up after a late
       run, so a starved task does not then run several times in a row. */
    step_sched_init(&sched, HOUSEKEEPING_PERIOD_MS,
                    osKernelGetTickCount() + HOUSEKEEPING_PERIOD_MS);

    for (;;) {
        (void)osDelayUntil(sched.boundary_ms);

        const uint32_t count = runs + 1u;
        masked_copy(&runs, &count, sizeof(runs));

        control_snapshot_t snap;
        control_step_read(&snap);
        /* The boundary, not the tick read now: a toggle every 100 ms or
           500 ms exactly, however late this low task gets the CPU. */
        if (heartbeat_update(&heartbeat, sched.boundary_ms, snap.step_count,
                             snap.fault_active)) {
            HAL_GPIO_TogglePin(GPIO_HBLED_GPIO_Port, GPIO_HBLED_Pin);
        }

        if (def != NULL && def->poll != NULL) {
            def->poll();
        }

        if (count % STATS_EVERY_RUNS == 0u) {
            stats_gather();
        }

        (void)step_sched_next(&sched, osKernelGetTickCount());
    }
}
