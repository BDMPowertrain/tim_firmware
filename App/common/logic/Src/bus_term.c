/* bus_term.c — see bus_term.h. */

#include "bus_term.h"

void bus_term_write_stored(bus_term_t *t, bool stored, uint32_t now_ms)
{
    t->stored = stored;
    t->relay = stored;
    t->written = true;
    t->written_ms = now_ms;
}

bool bus_term_settled(const bus_term_t *t, uint32_t now_ms)
{
    /* Unsigned subtraction: right across the tick counter's wrap. */
    return t->written && (now_ms - t->written_ms) > BUS_TERM_SETTLE_MS;
}

void bus_term_override(bus_term_t *t, bool on)
{
    t->relay = on;
}

bool bus_term_overridden(const bus_term_t *t)
{
    return t->relay != t->stored;
}
