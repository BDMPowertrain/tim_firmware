/*
 * The Control step's watch on Housekeeping (issue 47; issue 36, "Housekeeping
 * bumps a counter; the step sets 'background stalled' if it stops for 1 s").
 *
 * Housekeeping, the Low-priority task, bumps a counter every 100 ms. The step
 * hands this the counter every period: if it has not moved for 1 s, the
 * background is stalled (something above Low is hogging the CPU, or
 * Housekeeping is stuck). It is a warning, reported on the DAQ bus, and it
 * ends when the counter moves again. It resets nothing.
 *
 * The first second is counted from the first call, so a step that starts
 * before Housekeeping has run raises nothing.
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef STALL_WATCH_H
#define STALL_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#define STALL_WATCH_LIMIT_MS 1000u

/* Zero-initialise at power-on. */
typedef struct {
    bool     started;
    uint32_t count;      /* as last seen */
    uint32_t moved_ms;   /* when it was last seen to move, or the first call */
} stall_watch_t;

/* True: the counter has not moved for STALL_WATCH_LIMIT_MS. */
bool stall_watch_update(stall_watch_t *w, uint32_t count, uint32_t now_ms);

#endif /* STALL_WATCH_H */
