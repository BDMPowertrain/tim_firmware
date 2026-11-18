/* control_step.c — see control_step.h. */

#include "control_step.h"

#include <string.h>

#include "cmsis_os2.h"

#include "cycle_counter.h"
#include "data_log_record.h"
#include "data_log_ring.h"
#include "event_codes.h"
#include "events.h"
#include "housekeeping.h"
#include "kick_gate.h"
#include "main.h"
#include "masked_copy.h"
#include "print.h"
#include "reset_info.h"
#include "stall_watch.h"
#include "watchdog.h"

/* A phase index is kept in the Breadcrumb beside its own markers. */
_Static_assert(STEP_PHASE_MAX <= BREADCRUMB_PHASE_END,
               "every phase index fits below the Breadcrumb's markers");

/* Written once by the task before its first step, read by the shell. */
static const control_step_def_t *runningDef;

static control_snapshot_t published;

/* The step's Record as it is built: the step's own, static rather than on
   its stack. */
static uint8_t recordBytes[CONTROL_STEP_RECORD_MAX];

/* Set by the shell, read by the step. */
static volatile bool hangRequested;

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
    const size_t imageDataSize = (def->image_data_size < CONTROL_IMAGE_DATA_MAX)
                                     ? def->image_data_size
                                     : CONTROL_IMAGE_DATA_MAX;

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

        /* The Breadcrumb (ADR 0013): no code runs before a watchdog reset,
           so where the step was is written as it goes, at the start of the
           step and of each phase. */
        breadcrumb_t crumb = {
            .step_count = ctx.step_count,
            .phase = BREADCRUMB_PHASE_START,
            .overruns = sched.overruns,
        };
        reset_info_breadcrumb(&crumb);

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
            crumb.phase = (uint8_t)i;
            reset_info_breadcrumb(&crumb);
            def->phases[i].run(&ctx);
            kick_gate_phase_done(&gate, i);
            const uint32_t phaseEnd = cycle_counter_read();
            phaseUs[i] = (phaseEnd - phaseStart) / cyclesPerUs;
            phaseStart = phaseEnd;
        }

        crumb.phase = BREADCRUMB_PHASE_END;
        reset_info_breadcrumb(&crumb);

        const uint32_t stepUs = (phaseStart - stepStart) / cyclesPerUs;
        const uint32_t endMs = osKernelGetTickCount();

        step_stats_record(&stats, stepUs, phaseUs, phaseCount);
        wake = step_sched_next(&sched, endMs);

        /* The Data log's Record of this step (ADR 0021): copied into the
           step ring, never waiting; a full ring drops it and counts. */
        const data_log_step_t record = {
            .step_count = ctx.step_count,
            .state = ctx.state,
            .fault_active = ctx.fault_active,
            .background_stalled = ctx.background_stalled,
            .timing_fault = ctx.timing_fault,
            .startup_ok = ctx.startup_ok,
            .step_us = stepUs,
            .overruns = sched.overruns,
        };
        uint8_t recordType = DATA_LOG_STEP;
        uint16_t recordLen = (def->record != NULL)
                                 ? def->record(&record, recordBytes, sizeof recordBytes,
                                               &recordType)
                                 : 0u;
        if (recordLen == 0u) {
            recordType = DATA_LOG_STEP;
            data_log_step_encode(recordBytes, &record);
            recordLen = DATA_LOG_STEP_SIZE;
        }
        (void)data_log_push(DATA_LOG_RING_STEP, recordType, ctx.now_ms, recordBytes, recordLen);

        ctx.step_count++;

        timingFault = false;
        if (sched.last_overran) {
            const overrun_event_t overrun = overrun_event_make(stepUs, phaseUs, phaseCount);
            uint8_t data[EVENT_DATA_SIZE];

            event_data_overrun(data, &overrun);
            if (!events_post(EVENT_OVERRUN, data)) {
                eventsDropped++;
            }
            timingFault = overrun_watch_record(&overrunWatch, endMs);
            if (timingFault) {
                timingFaults++;
            }
        }

        control_snapshot_t snapshot = {
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
        if (def->image_data != NULL) {
            memcpy(snapshot.image_data.bytes, def->image_data, imageDataSize);
        }
        masked_copy(&published, &snapshot, sizeof(published));
    }
}

const char *control_step_phase_name(size_t index)
{
    const control_step_def_t *def = runningDef;

    if (def == NULL || index >= def->phase_count || index >= STEP_PHASE_MAX) {
        return NULL;
    }
    return def->phases[index].name;
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
    shell_printf("events queued %lu; overrun events dropped %lu\r\n",
                 (unsigned long)events_queued(), (unsigned long)s.events_dropped);
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
