#include "heartbeat.h"

bool heartbeat_update(heartbeat_t *hb, uint32_t now_ms, uint32_t step_count,
                      bool fault_active)
{
    const uint32_t halfPeriod = fault_active ? HEARTBEAT_FAULT_HALF_PERIOD_MS
                                             : HEARTBEAT_NORMAL_HALF_PERIOD_MS;

    /* A stalled step stops the LED wherever it is. */
    if (step_count == hb->step_count_at_toggle) {
        return false;
    }
    if ((now_ms - hb->last_toggle_ms) < halfPeriod) {
        return false;
    }

    hb->last_toggle_ms = now_ms;
    hb->step_count_at_toggle = step_count;
    return true;
}
