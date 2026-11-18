/* bq_frame.c — see bq_frame.h. */

#include "bq_frame.h"

#include <stdbool.h>
#include <string.h>

#define CRC_POLY_REFLECTED 0xA001u /* 0x8005 bit-reversed */
#define WRITE_MAX          8u
#define READ_MAX           128u
#define INIT_COMMAND       0x80u
#define INIT_COUNT_MASK    0x7Fu

uint16_t bq_crc(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (unsigned b = 0u; b < 8u; b++) {
            crc = ((crc & 1u) != 0u) ? (uint16_t)((crc >> 1) ^ CRC_POLY_REFLECTED)
                                     : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* INIT, the device address for a single device command, the register
   address; returns the bytes written. */
static size_t put_header(uint8_t *out, uint8_t init, bool single, uint8_t dev, uint16_t reg)
{
    size_t n = 0u;

    out[n++] = init;
    if (single) {
        out[n++] = dev;
    }
    out[n++] = (uint8_t)(reg >> 8);
    out[n++] = (uint8_t)reg;
    return n;
}

static size_t seal(uint8_t *out, size_t n)
{
    const uint16_t crc = bq_crc(out, n);

    out[n++] = (uint8_t)crc;
    out[n++] = (uint8_t)(crc >> 8);
    return n;
}

size_t bq_frame_write(uint8_t out[BQ_FRAME_MAX], bq_request_t type, uint8_t dev,
                      uint16_t reg, const uint8_t *data, size_t n)
{
    if (n == 0u || n > WRITE_MAX) {
        return 0u;
    }
    if (type != BQ_SINGLE_WRITE && type != BQ_STACK_WRITE && type != BQ_BROADCAST_WRITE) {
        return 0u;
    }

    size_t len = put_header(out, (uint8_t)((unsigned)type | (n - 1u)),
                            type == BQ_SINGLE_WRITE, dev, reg);
    memcpy(&out[len], data, n);
    return seal(out, len + n);
}

size_t bq_frame_read(uint8_t out[BQ_FRAME_MAX], bq_request_t type, uint8_t dev,
                     uint16_t reg, size_t count)
{
    if (count == 0u || count > READ_MAX) {
        return 0u;
    }
    if (type != BQ_SINGLE_READ && type != BQ_STACK_READ) {
        return 0u;
    }

    size_t len = put_header(out, (uint8_t)type, type == BQ_SINGLE_READ, dev, reg);
    out[len++] = (uint8_t)(count - 1u);
    return seal(out, len);
}

bq_response_status_t bq_response_next(const uint8_t *buf, size_t len, size_t *pos,
                                      bq_response_t *out)
{
    const size_t start = *pos;

    if (start >= len) {
        return BQ_RESPONSE_SHORT;
    }
    if ((buf[start] & INIT_COMMAND) != 0u) {
        return BQ_RESPONSE_NOT_A_RESPONSE;
    }

    const size_t count = (size_t)(buf[start] & INIT_COUNT_MASK) + 1u;
    const size_t frameLen = count + BQ_RESPONSE_OVERHEAD;
    if (len - start < frameLen) {
        return BQ_RESPONSE_SHORT;
    }
    if (bq_crc(&buf[start], frameLen) != 0u) {
        return BQ_RESPONSE_BAD_CRC;
    }

    out->dev = buf[start + 1u];
    out->reg = (uint16_t)(((unsigned)buf[start + 2u] << 8) | buf[start + 3u]);
    out->data = &buf[start + 4u];
    out->len = count;
    *pos = start + frameLen;
    return BQ_RESPONSE_OK;
}
