/* data_log_record.c — the Data log's file format, see data_log_record.h. */

#include "data_log_record.h"

#include <string.h>

#include "crc16.h"

static void put_u16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *b, uint32_t v)
{
    put_u16(b, (uint16_t)v);
    put_u16(&b[2], (uint16_t)(v >> 16));
}

void data_log_header_encode(uint8_t out[DATA_LOG_HEADER_SIZE], const data_log_header_t *h)
{
    memset(out, 0, DATA_LOG_HEADER_SIZE);
    put_u32(&out[0], DATA_LOG_MAGIC);
    put_u16(&out[4], DATA_LOG_LAYOUT);
    out[6] = h->image;
    out[7] = h->dirty ? 0x01u : 0x00u;
    put_u32(&out[8], h->build_id);
    put_u16(&out[12], h->boot);
    put_u16(&out[14], h->file);
    put_u32(&out[16], h->opened_ms);
    put_u16(&out[30], crc16(out, 30u));
}

size_t data_log_record_size(uint16_t len)
{
    return DATA_LOG_RECORD_OVERHEAD + (size_t)len;
}

size_t data_log_record_encode(uint8_t *out, size_t cap, const record_head_t *head,
                              const uint8_t *payload)
{
    const size_t size = data_log_record_size(head->len);

    if (size > cap) {
        return 0u;
    }
    out[0] = DATA_LOG_SYNC;
    out[1] = head->type;
    put_u16(&out[2], head->len);
    put_u32(&out[4], head->uptime_ms);
    put_u32(&out[8], head->seq);
    if (head->len > 0u) {
        memcpy(&out[12], payload, head->len);
    }
    put_u16(&out[12u + head->len], crc16(out, 12u + (size_t)head->len));
    return size;
}

void data_log_step_encode(uint8_t out[DATA_LOG_STEP_SIZE], const data_log_step_t *s)
{
    const uint8_t flags = (uint8_t)((s->fault_active ? 0x01u : 0u) |
                                    (s->background_stalled ? 0x02u : 0u) |
                                    (s->timing_fault ? 0x04u : 0u) |
                                    (s->startup_ok ? 0x08u : 0u));

    put_u32(&out[0], s->step_count);
    out[4] = s->state;
    out[5] = flags;
    put_u16(&out[6], s->step_us > 0xFFFFu ? 0xFFFFu : (uint16_t)s->step_us);
    put_u32(&out[8], s->overruns);
}

void data_log_imu_encode(uint8_t out[DATA_LOG_IMU_SIZE], const data_log_imu_t *s)
{
    for (size_t i = 0; i < 3u; i++) {
        put_u16(&out[2u * i], (uint16_t)s->accel[i]);
        put_u16(&out[6u + 2u * i], (uint16_t)s->gyro[i]);
    }
}

uint16_t data_log_daq_encode(uint8_t out[DATA_LOG_DAQ_SIZE_MAX], const can_frame_t *f)
{
    const uint8_t len = (f->len > CAN_FRAME_DATA_MAX) ? (uint8_t)CAN_FRAME_DATA_MAX : f->len;
    const uint32_t id = (f->id & 0x1FFFFFFFu) | (f->is_extended ? DATA_LOG_ID_EXTENDED : 0u) |
                        (f->is_fd ? DATA_LOG_DAQ_FD : 0u) | (f->is_brs ? DATA_LOG_DAQ_BRS : 0u);

    put_u32(&out[0], id);
    out[4] = len;
    memcpy(&out[5], f->data, len);
    return (uint16_t)(5u + len);
}
