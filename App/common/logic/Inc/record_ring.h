/*
 * The Data log ring (ADR 0021, issue 53): the hand-over of Records from one
 * producer, such as the Control step, to the SD logger task. Exactly one
 * task pushes and exactly one task pops; there is no lock and no masked
 * section. The writer copies a Record into the next free slot and only
 * then advances its index; the reader copies the oldest out and only then
 * advances its own. Each index is written by one side alone.
 *
 * The writer never waits: a full ring drops the new Record and counts it.
 * Every push, kept or dropped, takes the next sequence number, so a gap in
 * the numbers the reader sees is a visible loss.
 *
 * The caller supplies the storage (RECORD_RING_STORAGE_SIZE()), so each
 * image sizes its own rings. The depth is a power of two (the largest that
 * fits), so the free-running indices, compared by subtraction, wrap
 * safely at 2^32.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock. The hand-over uses
 * the compiler's __atomic built-ins, which are plain loads and stores with
 * barriers on a single-core Cortex-M4.
 */
#ifndef RECORD_RING_H
#define RECORD_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the reader gets with each Record's payload. */
typedef struct {
    uint32_t uptime_ms;
    uint32_t seq;  /* this ring's, from 0 at init */
    uint16_t len;  /* payload bytes */
    uint8_t  type; /* data_log_record.h */
} record_head_t;

/* One slot: the head, then the payload, rounded up to a word. */
#define RECORD_RING_SLOT_SIZE(payload_max) \
    ((sizeof(record_head_t) + (size_t)(payload_max) + 3u) & ~(size_t)3u)

/* Storage for depth slots of payload_max bytes each. */
#define RECORD_RING_STORAGE_SIZE(depth, payload_max) \
    ((size_t)(depth) * RECORD_RING_SLOT_SIZE(payload_max))

typedef struct {
    uint8_t *storage;
    size_t   slot_size;
    uint16_t payload_max;
    uint32_t depth;

    uint32_t head;     /* written by the writer only: Records pushed and kept */
    uint32_t tail;     /* written by the reader only: Records popped */
    uint32_t next_seq; /* the writer's */
    uint32_t dropped;  /* the writer's: full, or too long for a slot */
} record_ring_t;

/* Lays the ring over storage (word-aligned): the largest power of two of
   slots of payload_max bytes that fits. Empty, nothing dropped, sequence
   numbers from 0. Call before either side runs. */
void record_ring_init(record_ring_t *r, void *storage, size_t storage_size,
                      uint16_t payload_max);

/* Slots in the ring. */
uint32_t record_ring_depth(const record_ring_t *r);

/* The writer: copies one Record in. Never waits. False if it was dropped:
   the ring was full, or len is over the slot's payload_max. */
bool record_ring_push(record_ring_t *r, uint8_t type, uint32_t uptime_ms,
                      const void *payload, uint16_t len);

/* The reader: copies the oldest Record out and frees its slot. False if
   the ring is empty. A payload longer than cap is not copied and comes out
   with len 0, so the Record still counts as taken. */
bool record_ring_pop(record_ring_t *r, record_head_t *head, void *payload, size_t cap);

/* Records dropped since init. Any task. */
uint32_t record_ring_dropped(const record_ring_t *r);

#endif /* RECORD_RING_H */
