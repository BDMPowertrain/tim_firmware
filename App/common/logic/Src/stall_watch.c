#include "stall_watch.h"

bool stall_watch_update(stall_watch_t *w, uint32_t count, uint32_t now_ms)
{
    if (!w->started || count != w->count) {
        w->started = true;
        w->count = count;
        w->moved_ms = now_ms;
        return false;
    }
    return (now_ms - w->moved_ms) >= STALL_WATCH_LIMIT_MS;
}
