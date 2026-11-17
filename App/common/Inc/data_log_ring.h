#ifndef APP_COMMON_INC_DATA_LOG_RING_H
#define APP_COMMON_INC_DATA_LOG_RING_H

/* data_log_ring — where Records wait for the SD logger (ADR 0021, issue
   53), and the logger's state for the DAQ publisher. Linked into every
   image, so the Control step and the Event writer can always push; only an
   image with a logger (data_log.h) opens the rings, and until then a push
   does nothing and counts nothing.

   Two rings (record_ring.h), each with one writer and the logger as its
   one reader, no lock and no masked section:
   - the step ring, written by the Control step once per step: the third
     way out of the step (ADR 0010). The image sizes it: 64 Records on the
     VCU (640 ms).
   - the Event ring, written by the Event writer with a copy of each Event
     as stored (event_log_encode()), DATA_LOG_EVENT_RING_DEPTH deep.

   A full ring drops the new Record and counts it; nothing here waits.

   This file never checks which image it is built into. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "data_log_card.h"
#include "event_log.h"
#include "record_ring.h"

#define DATA_LOG_EVENT_RING_DEPTH 16u

/* Opens both rings, the step ring over the image's storage. Call from
   tasks_init() before any task that pushes is created. False if the
   storage holds no slot. */
bool data_log_rings_open(void *step_storage, size_t step_storage_size,
                         uint16_t step_payload_max);

/* The Control step: one Record. Never waits. False if dropped, or if no
   logger opened the rings. */
bool data_log_push_step(uint8_t type, uint32_t uptime_ms, const void *payload, uint16_t len);

/* The Event writer: a copy of one Event as stored. Never waits. */
void data_log_push_event(const uint8_t event[EVENT_SIZE], uint32_t uptime_ms);

/* The logger: the rings, NULL until opened. */
record_ring_t *data_log_step_ring(void);
record_ring_t *data_log_event_ring(void);

/* Records dropped at either ring since power-on. Any task. */
uint32_t data_log_dropped(void);

/* The card's state: set by the logger, read by any task. WAITING in an
   image with no logger. */
void data_log_set_state(data_log_state_t state);
data_log_state_t data_log_state(void);

#endif /* APP_COMMON_INC_DATA_LOG_RING_H */
