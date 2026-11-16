/* control_step.c — see control_step.h. */

#include "control_step.h"

#include "cmsis_os2.h"

#include "main.h"
#include "masked_copy.h"
#include "print.h"

/* Written once by the task before its first step, read by the shell. */
static const control_step_def_t *runningDef;

static control_snapshot_t published;

/* The DWT cycle counter: 170 MHz, wraps every 25 s, so a difference
   across one step is always right. */
static void cycle_counter_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
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

    cycle_counter_init();
    runningDef = def;

    step_sched_init(&sched, def->period_ms, osKernelGetTickCount() + def->period_ms);
    uint32_t wake = sched.boundary_ms;

    for (;;) {
        (void)osDelayUntil(wake);

        ctx.now_ms = osKernelGetTickCount();
        const uint32_t stepStart = DWT->CYCCNT;
        uint32_t phaseStart = stepStart;

        for (size_t i = 0; i < phaseCount; i++) {
            def->phases[i].run(&ctx);
            const uint32_t phaseEnd = DWT->CYCCNT;
            phaseUs[i] = (phaseEnd - phaseStart) / cyclesPerUs;
            phaseStart = phaseEnd;
        }

        step_stats_record(&stats, (phaseStart - stepStart) / cyclesPerUs, phaseUs, phaseCount);
        wake = step_sched_next(&sched, osKernelGetTickCount());
        ctx.step_count++;

        const control_snapshot_t snapshot = {
            .running = true,
            .step_count = ctx.step_count,
            .state = ctx.state,
            .startup_ok = ctx.startup_ok,
            .overruns = sched.overruns,
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
    shell_printf("overruns %lu\r\n", (unsigned long)s.overruns);
    shell_printf("%-8s last %5lu us  worst %5lu us\r\n", "step",
                 (unsigned long)s.stats.last_step_us, (unsigned long)s.stats.worst_step_us);
    for (size_t i = 0; i < def->phase_count && i < STEP_PHASE_MAX; i++) {
        shell_printf("%-8s last %5lu us  worst %5lu us\r\n", def->phases[i].name,
                     (unsigned long)s.stats.last_phase_us[i],
                     (unsigned long)s.stats.worst_phase_us[i]);
    }
}
