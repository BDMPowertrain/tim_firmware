#include "bus_recovery.h"

/* How long the bus has been running clean at now_ms, or 0 if it is not
 * running. */
static uint32_t clean_ms(const bus_recovery_t *br, uint32_t now_ms)
{
    return br->running ? (now_ms - br->running_since_ms) : 0u;
}

/* One bus-off seen at now_ms: continue or start the run, and latch on the
 * third of a run. */
static void count_bus_off(bus_recovery_t *br, uint32_t now_ms)
{
    br->counters.bus_offs++;

    if (clean_ms(br, now_ms) >= BUS_RECOVERY_CLEAN_MS) {
        br->quick_bus_offs = 0u;
    }
    br->running = false;
    if (br->quick_bus_offs < UINT8_MAX) {
        br->quick_bus_offs++;
    }
    if (br->quick_bus_offs >= BUS_RECOVERY_LATCH_BUS_OFFS) {
        br->latched = true;
    }
}

bus_recovery_action_t bus_recovery_update(bus_recovery_t *br,
                                          const bus_recovery_input_t *in,
                                          uint32_t now_ms)
{
    bus_recovery_action_t action = {0};
    const uint32_t new_bus_offs = in->bus_off_count - br->seen_bus_off_count;

    if (in->error_passive && !br->error_passive) {
        br->counters.error_passives++;
    }
    br->error_passive = in->error_passive;

    if (new_bus_offs != 0u) {
        br->seen_bus_off_count = in->bus_off_count;
        for (uint32_t i = 0; i < new_bus_offs; i++) {
            count_bus_off(br, now_ms);
        }
        br->off = true;
        br->off_since_ms = now_ms;
        action.bus_off = true;
        return action;
    }

    if (br->off) {
        const uint32_t wait_ms = br->latched ? BUS_RECOVERY_SLOW_RESTART_MS
                                             : BUS_RECOVERY_RESTART_MS;
        if ((now_ms - br->off_since_ms) >= wait_ms) {
            br->off = false;
            br->counters.restarts++;
            action.restart = true;
        }
        return action;
    }

    /* Restarted: clean time starts once the controller has rejoined. */
    if (!br->running && !in->controller_off) {
        br->running = true;
        br->running_since_ms = now_ms;
    }

    if (br->latched && in->latch_may_clear &&
        clean_ms(br, now_ms) >= BUS_RECOVERY_UNLATCH_MS) {
        br->latched = false;
        br->quick_bus_offs = 0u;
    }

    return action;
}
