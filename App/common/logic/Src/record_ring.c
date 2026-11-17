/* record_ring.c — the Data log ring, see record_ring.h. */

#include "record_ring.h"

#include <string.h>

static uint8_t *slot(const record_ring_t *r, uint32_t index)
{
    return &r->storage[(size_t)(index % r->depth) * r->slot_size];
}

void record_ring_init(record_ring_t *r, void *storage, size_t storage_size,
                      uint16_t payload_max)
{
    memset(r, 0, sizeof *r);
    r->storage = (uint8_t *)storage;
    r->payload_max = payload_max;
    r->slot_size = RECORD_RING_SLOT_SIZE(payload_max);

    /* A power of two, so index % depth stays continuous when the
       free-running indices wrap at 2^32. */
    const size_t slots = storage_size / r->slot_size;
    uint32_t depth = 1u;

    while ((size_t)depth * 2u <= slots && depth < 0x80000000u) {
        depth *= 2u;
    }
    r->depth = slots > 0u ? depth : 0u;
}

uint32_t record_ring_depth(const record_ring_t *r)
{
    return r->depth;
}

bool record_ring_push(record_ring_t *r, uint8_t type, uint32_t uptime_ms,
                      const void *payload, uint16_t len)
{
    const uint32_t seq = r->next_seq++;
    const uint32_t head = r->head;
    /* Acquire: the reader has finished copying out of a slot before the
       writer sees it free. */
    const uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);

    if (len > r->payload_max || r->depth == 0u || head - tail >= r->depth) {
        __atomic_store_n(&r->dropped, r->dropped + 1u, __ATOMIC_RELAXED);
        return false;
    }

    uint8_t *s = slot(r, head);
    const record_head_t h = {
        .uptime_ms = uptime_ms,
        .seq = seq,
        .len = len,
        .type = type,
    };

    memcpy(s, &h, sizeof h);
    if (len > 0u) {
        memcpy(&s[sizeof h], payload, len);
    }
    /* Release: the copy is complete before the reader sees the slot. */
    __atomic_store_n(&r->head, head + 1u, __ATOMIC_RELEASE);
    return true;
}

bool record_ring_pop(record_ring_t *r, record_head_t *head, void *payload, size_t cap)
{
    const uint32_t tail = r->tail;
    const uint32_t full = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);

    if (tail == full) {
        return false;
    }

    const uint8_t *s = slot(r, tail);

    memcpy(head, s, sizeof *head);
    if (head->len > cap) {
        head->len = 0u;
    } else if (head->len > 0u) {
        memcpy(payload, &s[sizeof *head], head->len);
    }
    __atomic_store_n(&r->tail, tail + 1u, __ATOMIC_RELEASE);
    return true;
}

uint32_t record_ring_dropped(const record_ring_t *r)
{
    return __atomic_load_n(&r->dropped, __ATOMIC_RELAXED);
}
