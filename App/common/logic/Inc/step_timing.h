/*
 * The Control step's timing (ADR 0010): when the next step starts, what an
 * Overrun is, and the worst case kept since power-on.
 *
 * Scheduling. The step runs on fixed period boundaries and waits for the
 * next one with the RTOS delay-until call. A step that is still running when
 * its next boundary begins is an Overrun: it is counted once, every boundary
 * it covered is lost, and the next step starts on the first boundary after
 * it finished. There is no catch-up.
 *
 * Statistics. The step times itself and each of its phases every run; the
 * last and the worst since power-on are kept here and published in the
 * step's snapshot.
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef STEP_TIMING_H
#define STEP_TIMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t period_ms;
    uint32_t boundary_ms;  /* the boundary the current step started on */
    uint32_t overruns;     /* Overruns since power-on */
    bool     last_overran; /* the step just finished was an Overrun */
} step_sched_t;

/* The first step starts on first_ms. period_ms is at least 1. */
void step_sched_init(step_sched_t *sched, uint32_t period_ms,
                     uint32_t first_ms);

/*
 * Call once a step has finished, with the time it finished. Returns the
 * boundary the next step starts on, for the delay-until call, and makes it
 * the current one. A step is an Overrun if now_ms has reached the boundary
 * after the one it started on.
 */
uint32_t step_sched_next(step_sched_t *sched, uint32_t now_ms);

#define STEP_PHASE_MAX 5u /* phases a step may time: the BMS has 5 (issue 64) */

/* Times in microseconds. Zero-initialise at power-on. */
typedef struct {
    uint32_t last_step_us;
    uint32_t worst_step_us;
    uint32_t last_phase_us[STEP_PHASE_MAX];
    uint32_t worst_phase_us[STEP_PHASE_MAX];
} step_stats_t;

/*
 * Records one step: its whole time and each phase's, in phase order.
 * Phases beyond STEP_PHASE_MAX are not kept.
 */
void step_stats_record(step_stats_t *stats, uint32_t step_us,
                       const uint32_t *phase_us, size_t phase_count);

#endif /* STEP_TIMING_H */
