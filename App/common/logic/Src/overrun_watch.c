#include "overrun_watch.h"

bool overrun_watch_record(overrun_watch_t *watch, uint32_t now_ms)
{
    const size_t kept = OVERRUN_FAULT_COUNT - 1u;
    const bool fault = (watch->seen >= kept)
                       && ((now_ms - watch->at_ms[0]) < OVERRUN_FAULT_WINDOW_MS);

    for (size_t i = 1u; i < kept; i++) {
        watch->at_ms[i - 1u] = watch->at_ms[i];
    }
    watch->at_ms[kept - 1u] = now_ms;
    if (watch->seen < kept) {
        watch->seen++;
    }
    return fault;
}

overrun_event_t overrun_event_make(uint32_t step_us, const uint32_t *phase_us,
                                   size_t phase_count, bool open_wire)
{
    overrun_event_t event = { .step_us = step_us, .open_wire = open_wire };

    for (size_t i = 0; i < phase_count; i++) {
        if (phase_us[i] > event.slowest_phase_us) {
            event.slowest_phase_us = phase_us[i];
            event.slowest_phase = (uint8_t)i;
        }
    }
    return event;
}
