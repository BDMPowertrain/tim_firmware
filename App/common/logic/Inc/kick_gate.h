/*
 * Whether the Control step may Kick the watchdog (ADR 0013).
 *
 * A step Kicks once, at its start, and only if the previous step ran every
 * phase in order to the end. The first step has no previous step and Kicks
 * unconditionally. A phase skipped, run twice or run out of turn, or a step
 * that never reached its last phase, withholds the next Kick; the watchdog
 * then resets the MCU unless a later Kick comes in time. Only the step that
 * follows a complete one Kicks: a gap is never made good.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef KICK_GATE_H
#define KICK_GATE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t phase_count;
    size_t next_phase; /* the phase whose turn it is */
    bool   in_order;   /* every phase so far ran in its turn */
    bool   started;    /* a step has begun */
} kick_gate_t;

/* phase_count is the number of phases every step runs, at least 1. */
void kick_gate_init(kick_gate_t *gate, size_t phase_count);

/* At the start of each step: true if this step may Kick. */
bool kick_gate_step_start(kick_gate_t *gate);

/* After phase `phase` (counted from 0) has returned. */
void kick_gate_phase_done(kick_gate_t *gate, size_t phase);

#endif /* KICK_GATE_H */
