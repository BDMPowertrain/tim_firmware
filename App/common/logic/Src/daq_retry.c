#include "daq_retry.h"

bool daq_retry_update(daq_retry_t *r, bool controller_off, uint32_t now_ms)
{
    if (!controller_off) {
        r->off = false;
        return false;
    }

    if (!r->off) {
        r->off = true;
        r->off_since_ms = now_ms;
        r->bus_offs++;
        return false;
    }

    if ((now_ms - r->off_since_ms) < DAQ_RETRY_MS) {
        return false;
    }

    /* Still off after this restart: the next one is a second from now. */
    r->off_since_ms = now_ms;
    r->restarts++;
    return true;
}
