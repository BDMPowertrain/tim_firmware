/* data_log_ring.c — see data_log_ring.h. */

#include "data_log_ring.h"

#include "data_log_record.h"

static record_ring_t stepRing;
static record_ring_t eventRing;

static uint32_t eventStorage[RECORD_RING_STORAGE_SIZE(DATA_LOG_EVENT_RING_DEPTH, EVENT_SIZE) /
                             sizeof(uint32_t)];

/* Set once in tasks_init(), before any pushing task exists. */
static bool opened;

/* Written by the logger only; one word, read whole. */
static volatile data_log_state_t cardState = DATA_LOG_WAITING;

bool data_log_rings_open(void *step_storage, size_t step_storage_size,
                         uint16_t step_payload_max)
{
    record_ring_init(&stepRing, step_storage, step_storage_size, step_payload_max);
    record_ring_init(&eventRing, eventStorage, sizeof eventStorage, EVENT_SIZE);
    opened = record_ring_depth(&stepRing) > 0u;
    return opened;
}

bool data_log_push_step(uint8_t type, uint32_t uptime_ms, const void *payload, uint16_t len)
{
    return opened && record_ring_push(&stepRing, type, uptime_ms, payload, len);
}

void data_log_push_event(const uint8_t event[EVENT_SIZE], uint32_t uptime_ms)
{
    if (opened) {
        (void)record_ring_push(&eventRing, DATA_LOG_EVENT, uptime_ms, event, EVENT_SIZE);
    }
}

record_ring_t *data_log_step_ring(void)
{
    return opened ? &stepRing : NULL;
}

record_ring_t *data_log_event_ring(void)
{
    return opened ? &eventRing : NULL;
}

uint32_t data_log_dropped(void)
{
    return opened ? record_ring_dropped(&stepRing) + record_ring_dropped(&eventRing) : 0u;
}

void data_log_set_state(data_log_state_t state)
{
    cardState = state;
}

data_log_state_t data_log_state(void)
{
    return cardState;
}
