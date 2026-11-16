/*
 * The DAQ bus send queue (issue 36, "DAQ bus isolation"): a ring of frames
 * that drops its oldest frame when full, so a caller never waits and the
 * newest data always gets a place. Every drop is counted.
 *
 * Not safe to share by itself: the caller makes each push and pop atomic
 * (in an image, a short critical section around a copy).
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef FRAME_QUEUE_H
#define FRAME_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "can_frame.h"

#define FRAME_QUEUE_DEPTH 16u

/* Zero-initialise: empty, nothing dropped. */
typedef struct {
    can_frame_t slots[FRAME_QUEUE_DEPTH];
    uint32_t    head;    /* the oldest frame */
    uint32_t    count;
    uint32_t    dropped; /* oldest frames pushed out by a newer one */
} frame_queue_t;

/* Adds frame at the back. A full queue first drops its oldest frame. */
void frame_queue_push(frame_queue_t *q, const can_frame_t *frame);

/* Takes the oldest frame into *out. False if the queue is empty. */
bool frame_queue_pop(frame_queue_t *q, can_frame_t *out);

#endif /* FRAME_QUEUE_H */
