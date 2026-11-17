/*
 * The Event log's flood guard (spec #36): a burst of EVENT_FLOOD_BURST
 * Events, refilled at one per EVENT_FLOOD_REFILL_MS, so that a fault that
 * fires every step cannot wear out the flash or push every older Event
 * out of the ring.
 *
 * An Event refused is counted, and so is one lost before it reached the
 * guard (a full queue, event_flood_add_dropped()). The count is reported
 * once, as one "N events dropped" Event, at the first chance after the
 * guard has a token again. The report itself takes no token: under a
 * steady flood the log gets at most one admitted Event and one report per
 * refill.
 *
 * Time is a 32-bit millisecond count compared only by subtraction.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef EVENT_FLOOD_H
#define EVENT_FLOOD_H

#include <stdbool.h>
#include <stdint.h>

#define EVENT_FLOOD_BURST     64u
#define EVENT_FLOOD_REFILL_MS 1000u

typedef struct {
    uint32_t tokens;    /* Events that may be admitted now */
    uint32_t refill_ms; /* when the last token was added, or the start */
    uint32_t dropped;   /* not yet reported */
} event_flood_t;

/* A full burst from now_ms. */
void event_flood_init(event_flood_t *f, uint32_t now_ms);

/* True if the Event may be written; false if it is dropped and counted. */
bool event_flood_admit(event_flood_t *f, uint32_t now_ms);

/* Counts n Events lost before they reached the guard. */
void event_flood_add_dropped(event_flood_t *f, uint32_t n);

/* The N of an "N events dropped" Event to write now, and the count
   restarts; 0 if nothing was dropped or the guard has no token yet. */
uint32_t event_flood_take_report(event_flood_t *f, uint32_t now_ms);

#endif /* EVENT_FLOOD_H */
