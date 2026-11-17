/*
 * Each task's CPU share (issue 47): its part of the run time counted since
 * the last gathering, from two samples of the RTOS's per-task run-time
 * counters. Housekeeping gathers once a second for the shell's `tasks`.
 *
 * Tasks are matched by their unique task number, not their position, since
 * the RTOS lists them in no fixed order. A rise is an unsigned difference,
 * so a counter that wrapped once in the window still counts. A task not in
 * the earlier sample (new, or the first gathering) counts from zero. The
 * window's total is the sum of every task's rise, so the shares need no
 * separate clock and add to about 1000.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef CPU_SHARE_H
#define CPU_SHARE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t task_number;
    uint32_t run_time; /* the task's counter, in any unit */
} cpu_sample_t;

/* share_permille[i] is now[i]'s share in tenths of a percent, rounded to
 * the nearest; every share is 0 if no time was counted. before may be NULL
 * when before_count is 0. */
void cpu_share_compute(const cpu_sample_t *before, size_t before_count,
                       const cpu_sample_t *now, size_t now_count,
                       uint32_t *share_permille);

#endif /* CPU_SHARE_H */
