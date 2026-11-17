/* control_step.c — see control_step.h. */

#include "control_step.h"

#include "cmsis_os2.h"

#include "cycle_counter.h"
#include "housekeeping.h"
#include "kick_gate.h"
#include "main.h"
#include "masked_copy.h"
#include "print.h"
#include "stall_watch.h"
#include "watchdog.h"

/* Written once by the task before its first step, read by the shell. */
static const control_step_def_t *runningDef;

static control_snapshot_t published;

/* Overrun Events the queue holds until the Event writer takes them. */
#define EVENT_QUEUE_DEPTH 16u

/* Overrun Events, for the Event writer (the Event log lands later; until
   then the queue fills and the rest are dropped and counted). */
static osMessageQueueId_t eventQueue;

/* Set by the shell, read by the step. */
static volatile bool hangRequested;

bool control_step_init(void)
{
    eventQueue = osMessageQueueNew(EVENT_QUEUE_DEPTH,
                                   sizeof(overrun_event_t), NULL);
    return eventQueue != NULL;
}

void control_step_request_hang(void)
{
    hangRequested = true;
}

void control_step_task(void *argument)
{
    const control_step_args_t *args = (const control_step_args_t *)argument;
    const control_step_def_t *def = args->def;
    const uint32_t cyclesPerUs = SystemCoreClock / 1000000u;
    const size_t phaseCount = (def->phase_count < STEP_PHASE_MAX) ? def->phase_count
                                                                   : STEP_PHASE_MAX;

    control_step_ctx_t ctx = {
        .startup_ok = args->startup_ok,
    };
    step_sched_t sched;
    step_stats_t stats = {0};
    uint32_t phaseUs[STEP_PHASE_MAX] = {0};
    stall_watch_t background = {0};
    kick_gate_t gate;
    overrun_watch_t overrunWatch = {0};
    uint32_t timingFaults = 0u;
    uint32_t kicksWithheld = 0u;
    uint32_t eventsDropped = 0u;
    bool timingFault = false;

    /* The cycle counter has run since the scheduler started (cycle_counter.h). */
    runningDef = def;
    kick_gate_init(&gate, phaseCount);

    step_sched_init(&sched, def->period_ms, osKernelGetTickCount() + def->period_ms);
    uint32_t wake = sched.boundary_ms;

    for (;;) {
        (void)osDelayUntil(wake);

        /* The one Kick once the step runs (ADR 0013). */
        if (kick_gate_step_start(&gate)) {
            watchdog_kick();
        } else {
            kicksWithheld++;
        }

        if (hangRequested) {
            /* The bench's hang: nothing below Realtime runs again and the
               watchdog resets the MCU one timeout after the Kick above. */
            for (;;) {
            }
        }

        ctx.now_ms = osKernelGetTickCount();
        ctx.timing_fault = timingFault;
        const uint32_t stepStart = cycle_counter_read();
        uint32_t phaseStart = stepStart;

        /* A warning only: nothing is reset or stopped for it. */
        ctx.background_stalled = stall_watch_update(&background, housekeeping_count(),
                                                    ctx.now_ms);

        for (size_t i = 0; i < phaseCount; i++) {
            def->phases[i].run(&ctx);
            kick_gate_phase_done(&gate, i);
            const uint32_t phaseEnd = cycle_counter_read();
            phaseUs[i] = (phaseEnd - phaseStart) / cyclesPerUs;
            phaseStart = phaseEnd;
        }

        const uint32_t stepUs = (phaseStart - stepStart) / cyclesPerUs;
        const uint32_t endMs = osKernelGetTickCount();

        step_stats_record(&stats, stepUs, phaseUs, phaseCount);
        wake = step_sched_next(&sched, endMs);
        ctx.step_count++;

        timingFault = false;
        if (sched.last_overran) {
            const overrun_event_t event = overrun_event_make(stepUs, phaseUs, phaseCount);

            if (osMessageQueuePut(eventQueue, &event, 0u, 0u) != osOK) {
                eventsDropped++;
            }
            timingFault = overrun_watch_record(&overrunWatch, endMs);
            if (timingFault) {
                timingFaults++;
            }
        }

        const control_snapshot_t snapshot = {
            .running = true,
            .step_count = ctx.step_count,
            .state = ctx.state,
            .startup_ok = ctx.startup_ok,
            .fault_active = ctx.fault_active,
            .background_stalled = ctx.background_stalled,
            .overruns = sched.overruns,
            .timing_faults = timingFaults,
            .kicks_withheld = kicksWithheld,
            .events_dropped = eventsDropped,
            .stats = stats,
            .bus = control_bus_report(),
        };
        masked_copy(&published, &snapshot, sizeof(published));
    }
}

void control_step_read(control_snapshot_t *out)
{
    masked_copy(out, &published, sizeof(*out));
}

void control_step_print(void)
{
    control_snapshot_t s;
    control_step_read(&s);

    const control_step_def_t *def = runningDef;
    if (def == NULL || !s.running) {
        shell_printf("control step: not running\r\n");
        return;
    }

    shell_printf("%s control step, %lu ms: state %s, %lu steps, startup %s\r\n",
                 def->image, (unsigned long)def->period_ms, def->state_name(s.state),
                 (unsigned long)s.step_count, s.startup_ok ? "ok" : "FAILED");
    shell_printf("overruns %lu, timing faults %lu, kicks withheld %lu\r\n",
                 (unsigned long)s.overruns, (unsigned long)s.timing_faults,
                 (unsigned long)s.kicks_withheld);
    shell_printf("overrun events queued %lu, dropped %lu\r\n",
                 (unsigned long)osMessageQueueGetCount(eventQueue),
                 (unsigned long)s.events_dropped);
    shell_printf("fault %s, background %s\r\n",
                 s.fault_active ? "ACTIVE" : "none",
                 s.background_stalled ? "STALLED" : "ok");
    shell_printf("%-8s last %5lu us  worst %5lu us\r\n", "step",
                 (unsigned long)s.stats.last_step_us, (unsigned long)s.stats.worst_step_us);
    for (size_t i = 0; i < def->phase_count && i < STEP_PHASE_MAX; i++) {
        shell_printf("%-8s last %5lu us  worst %5lu us\r\n", def->phases[i].name,
                     (unsigned long)s.stats.last_phase_us[i],
                     (unsigned long)s.stats.worst_phase_us[i]);
    }
}
