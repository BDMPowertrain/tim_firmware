#include "kick_gate.h"

void kick_gate_init(kick_gate_t *gate, size_t phase_count)
{
    gate->phase_count = phase_count;
    gate->next_phase = 0u;
    gate->in_order = true;
    gate->started = false;
}

bool kick_gate_step_start(kick_gate_t *gate)
{
    const bool complete = gate->in_order && (gate->next_phase == gate->phase_count);
    const bool kick = !gate->started || complete;

    gate->started = true;
    gate->next_phase = 0u;
    gate->in_order = true;
    return kick;
}

void kick_gate_phase_done(kick_gate_t *gate, size_t phase)
{
    /* A phase out of its turn, or one after the last, spoils the step for
       good: next_phase stops moving, so no later call can put it right. */
    if (!gate->in_order || phase != gate->next_phase || phase >= gate->phase_count) {
        gate->in_order = false;
        return;
    }
    gate->next_phase++;
}
