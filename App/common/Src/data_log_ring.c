/* data_log_ring.c — see data_log_ring.h. */

#include "data_log_ring.h"

#include "data_log_record.h"

static record_ring_t rings[DATA_LOG_RING_COUNT];

/* Set once in tasks_init(), before any pushing task exists. */
static bool present[DATA_LOG_RING_COUNT];

static uint32_t eventStorage[RECORD_RING_STORAGE_SIZE(DATA_LOG_EVENT_RING_DEPTH, EVENT_SIZE) /
                             sizeof(uint32_t)];

/* Written by the logger only; one word, read whole. */
static volatile data_log_state_t cardState = DATA_LOG_WAITING;

bool data_log_rings_open(const data_log_ring_def_t defs[DATA_LOG_RING_COUNT])
{
    for (size_t i = 0; i < DATA_LOG_RING_COUNT; i++) {
        if (i == DATA_LOG_RING_EVENT) {
            record_ring_init(&rings[i], eventStorage, sizeof eventStorage, EVENT_SIZE);
        } else if (defs[i].storage != NULL) {
            record_ring_init(&rings[i], defs[i].storage, defs[i].size, defs[i].payload_max);
        } else {
            continue;
        }
        present[i] = record_ring_depth(&rings[i]) > 0u;
    }
    /* Without the step ring, no ring is opened: nothing is logged. */
    if (!present[DATA_LOG_RING_STEP]) {
        for (size_t i = 0; i < DATA_LOG_RING_COUNT; i++) {
            present[i] = false;
        }
        return false;
    }
    return true;
}

bool data_log_push(data_log_ring_id_t ring, uint8_t type, uint32_t uptime_ms,
                   const void *payload, uint16_t len)
{
    return ring < DATA_LOG_RING_COUNT && present[ring] &&
           record_ring_push(&rings[ring], type, uptime_ms, payload, len);
}

void data_log_push_event(const uint8_t event[EVENT_SIZE], uint32_t uptime_ms)
{
    (void)data_log_push(DATA_LOG_RING_EVENT, DATA_LOG_EVENT, uptime_ms, event, EVENT_SIZE);
}

void data_log_push_daq(bool sent, const can_frame_t *frame, uint32_t uptime_ms)
{
    const data_log_ring_id_t ring = sent ? DATA_LOG_RING_DAQ_TX : DATA_LOG_RING_DAQ_RX;
    uint8_t payload[DATA_LOG_DAQ_SIZE_MAX];

    if (!present[ring]) {
        return; /* no encoding for a ring the image does not have */
    }
    const uint16_t len = data_log_daq_encode(payload, frame);
    (void)record_ring_push(&rings[ring], sent ? DATA_LOG_DAQ_TX : DATA_LOG_DAQ_RX, uptime_ms,
                           payload, len);
}

record_ring_t *data_log_ring(data_log_ring_id_t ring)
{
    return (ring < DATA_LOG_RING_COUNT && present[ring]) ? &rings[ring] : NULL;
}

uint32_t data_log_dropped(void)
{
    uint32_t dropped = 0u;

    for (size_t i = 0; i < DATA_LOG_RING_COUNT; i++) {
        if (present[i]) {
            dropped += record_ring_dropped(&rings[i]);
        }
    }
    return dropped;
}

void data_log_set_state(data_log_state_t state)
{
    cardState = state;
}

data_log_state_t data_log_state(void)
{
    return cardState;
}
