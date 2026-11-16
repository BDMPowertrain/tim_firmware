#include "frame_queue.h"

void frame_queue_push(frame_queue_t *q, const can_frame_t *frame)
{
    if (q->count == FRAME_QUEUE_DEPTH) {
        q->head = (q->head + 1u) % FRAME_QUEUE_DEPTH;
        q->count--;
        q->dropped++;
    }
    q->slots[(q->head + q->count) % FRAME_QUEUE_DEPTH] = *frame;
    q->count++;
}

bool frame_queue_pop(frame_queue_t *q, can_frame_t *out)
{
    if (q->count == 0u) {
        return false;
    }
    *out = q->slots[q->head];
    q->head = (q->head + 1u) % FRAME_QUEUE_DEPTH;
    q->count--;
    return true;
}
