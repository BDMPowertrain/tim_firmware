#include "step_timing.h"

void step_sched_init(step_sched_t *sched, uint32_t period_ms, uint32_t first_ms)
{
    sched->period_ms = period_ms;
    sched->boundary_ms = first_ms;
    sched->overruns = 0u;
    sched->last_overran = false;
}

uint32_t step_sched_next(step_sched_t *sched, uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - sched->boundary_ms;

    sched->last_overran = (elapsed >= sched->period_ms);
    if (sched->last_overran) {
        sched->overruns++;
    }

    /* The first boundary after now_ms. */
    const uint32_t periods = (elapsed / sched->period_ms) + 1u;
    sched->boundary_ms += periods * sched->period_ms;
    return sched->boundary_ms;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return (a > b) ? a : b;
}

void step_stats_record(step_stats_t *stats, uint32_t step_us,
                       const uint32_t *phase_us, size_t phase_count)
{
    stats->last_step_us = step_us;
    stats->worst_step_us = max_u32(stats->worst_step_us, step_us);

    if (phase_count > STEP_PHASE_MAX) {
        phase_count = STEP_PHASE_MAX;
    }
    for (size_t i = 0; i < phase_count; i++) {
        stats->last_phase_us[i] = phase_us[i];
        stats->worst_phase_us[i] = max_u32(stats->worst_phase_us[i], phase_us[i]);
    }
}
