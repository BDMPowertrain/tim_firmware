#ifndef APP_COMMON_INC_DATA_LOG_RING_H
#define APP_COMMON_INC_DATA_LOG_RING_H

/* data_log_ring — where Records wait for the SD logger (ADR 0021, issues
   53 and 61), and the logger's state for the DAQ publisher. Linked into
   every image, so every producer can always push; only an image with a
   logger (data_log.h) opens the rings, and until then a push does nothing
   and counts nothing.

   One ring (record_ring.h) per producer, each with that one writer and the
   logger as its one reader, no lock and no masked section:
   - STEP, written by the Control step once per step: the third way out of
     the step (ADR 0010). 64 Records on the VCU (640 ms).
   - EVENT, written by the Event writer with a copy of each Event as
     stored (event_log_encode()), DATA_LOG_EVENT_RING_DEPTH deep. Its
     storage is this file's own.
   - IMU, written by the IMU task: accel and gyro pairs, about 100 Hz.
   - DAQ_RX and DAQ_TX, written by canRxTask with each DAQ bus frame
     received and by daqTask with each frame handed to the controller,
     raw. Two rings, so each has one writer.
   The image gives the storage of each ring it fills; one it leaves out is
   absent, and a push to it does nothing and counts nothing, so common
   code pushes without asking which image it is in.

   A full ring drops the new Record and counts it; nothing here waits. A
   drop is also a gap in that type's sequence numbers in the file.

   This file never checks which image it is built into. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can_frame.h"
#include "data_log_card.h"
#include "event_log.h"
#include "record_ring.h"

#define DATA_LOG_EVENT_RING_DEPTH 16u

typedef enum {
    DATA_LOG_RING_STEP,
    DATA_LOG_RING_EVENT,
    DATA_LOG_RING_IMU,
    DATA_LOG_RING_DAQ_RX,
    DATA_LOG_RING_DAQ_TX,
    DATA_LOG_RING_COUNT
} data_log_ring_id_t;

/* One ring's storage (RECORD_RING_STORAGE_SIZE(), word-aligned). storage
   NULL: the image has no such ring. */
typedef struct {
    void    *storage;
    size_t   size;
    uint16_t payload_max;
} data_log_ring_def_t;

/* Opens the rings over the image's storage; defs[DATA_LOG_RING_EVENT] is
   not read. Call from tasks_init() before any task that pushes is
   created. False if the step ring holds no slot. */
bool data_log_rings_open(const data_log_ring_def_t defs[DATA_LOG_RING_COUNT]);

/* The ring's producer: one Record. Never waits. False if dropped, or if
   the ring is absent. */
bool data_log_push(data_log_ring_id_t ring, uint8_t type, uint32_t uptime_ms,
                   const void *payload, uint16_t len);

/* The Event writer: a copy of one Event as stored. Never waits. */
void data_log_push_event(const uint8_t event[EVENT_SIZE], uint32_t uptime_ms);

/* canRxTask (sent false) or daqTask (sent true): one DAQ bus frame, raw
   (data_log_daq_encode()). Never waits. */
void data_log_push_daq(bool sent, const can_frame_t *frame, uint32_t uptime_ms);

/* The logger: a ring, NULL if absent or not opened. */
record_ring_t *data_log_ring(data_log_ring_id_t ring);

/* Records dropped at every ring since power-on. Any task. */
uint32_t data_log_dropped(void);

/* The card's state: set by the logger, read by any task. WAITING in an
   image with no logger. */
void data_log_set_state(data_log_state_t state);
data_log_state_t data_log_state(void);

#endif /* APP_COMMON_INC_DATA_LOG_RING_H */
