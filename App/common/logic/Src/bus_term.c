/* bus_term.c — see bus_term.h. */

#include "bus_term.h"

void bus_term_write_stored(bus_term_t *t, bool stored)
{
    t->stored = stored;
    t->relay = stored;
    t->written = true;
}

void bus_term_override(bus_term_t *t, bool on)
{
    t->relay = on;
}

bool bus_term_overridden(const bus_term_t *t)
{
    return t->relay != t->stored;
}
