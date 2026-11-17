/* event_flood.c — the Event log's flood guard, see event_flood.h. */

#include "event_flood.h"

void event_flood_init(event_flood_t *f, uint32_t now_ms)
{
    f->tokens = EVENT_FLOOD_BURST;
    f->refill_ms = now_ms;
    f->dropped = 0u;
}

/* One token per whole refill period since the last, the remainder carried;
   a full burst stops the clock. */
static void refill(event_flood_t *f, uint32_t now_ms)
{
    const uint32_t periods = (now_ms - f->refill_ms) / EVENT_FLOOD_REFILL_MS;

    if (f->tokens + periods >= EVENT_FLOOD_BURST || periods >= EVENT_FLOOD_BURST) {
        f->tokens = EVENT_FLOOD_BURST;
        f->refill_ms = now_ms;
    } else {
        f->tokens += periods;
        f->refill_ms += periods * EVENT_FLOOD_REFILL_MS;
    }
}

bool event_flood_admit(event_flood_t *f, uint32_t now_ms)
{
    refill(f, now_ms);
    if (f->tokens == 0u) {
        f->dropped++;
        return false;
    }
    f->tokens--;
    return true;
}

void event_flood_add_dropped(event_flood_t *f, uint32_t n)
{
    f->dropped += n;
}

uint32_t event_flood_take_report(event_flood_t *f, uint32_t now_ms)
{
    refill(f, now_ms);
    if (f->tokens == 0u) {
        return 0u;
    }

    const uint32_t n = f->dropped;

    f->dropped = 0u;
    return n;
}
