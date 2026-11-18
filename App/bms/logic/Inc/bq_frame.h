/*
 * The cell-monitor chain's frames (issue 62): the bytes the TIM sends the
 * BQ79600-Q1 bridge, and the response frames that come back from the bridge
 * and the BQ79616-Q1 stack devices.
 *
 * A command frame is INIT, a device address (single device commands only),
 * a 2-byte register address, the data (1-8 bytes to write, or for a read
 * one byte holding the count - 1) and a 2-byte CRC (BQ79600-Q1 data sheet
 * SLUSDS1A Fig 7-7; BQ79600-Q1 Software Design Reference SLUAA17A 1.1).
 * A response frame is INIT (bit 7 clear, count - 1 below it), the device
 * address, the 2-byte register address, 1-128 data bytes and the CRC
 * (BQ79616-Q1 data sheet SLUSE81F Table 7-10).
 *
 * The CRC is CRC-16-IBM, reflected, initialised to 0xFFFF and sent low byte
 * first (CRC-16/MODBUS); over a whole frame including its CRC it is zero.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef BQ_FRAME_H
#define BQ_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* The longest command frame: a single device write of 8 bytes. */
#define BQ_FRAME_MAX 14u

/* Response bytes around the data of one response frame. */
#define BQ_RESPONSE_OVERHEAD 6u

/* The command's INIT byte, without its data size (SLUAA17A 1.1.1). */
typedef enum {
    BQ_SINGLE_READ     = 0x80,
    BQ_SINGLE_WRITE    = 0x90,
    BQ_STACK_READ      = 0xA0,
    BQ_STACK_WRITE     = 0xB0,
    BQ_BROADCAST_WRITE = 0xD0,
} bq_request_t;

uint16_t bq_crc(const uint8_t *data, size_t len);

/* Builds a write of n bytes (1 to 8) to reg into out. dev is used by
   BQ_SINGLE_WRITE only. Returns the frame length, or 0 if n or type is not
   a write. */
size_t bq_frame_write(uint8_t out[BQ_FRAME_MAX], bq_request_t type, uint8_t dev,
                      uint16_t reg, const uint8_t *data, size_t n);

/* Builds a read of count bytes (1 to 128) from reg into out, each
   responding device returning count bytes. dev is used by BQ_SINGLE_READ
   only. Returns the frame length, or 0 if count or type is not a read. */
size_t bq_frame_read(uint8_t out[BQ_FRAME_MAX], bq_request_t type, uint8_t dev,
                     uint16_t reg, size_t count);

typedef enum {
    BQ_RESPONSE_OK,
    /* Fewer bytes than the frame needs, or none left. */
    BQ_RESPONSE_SHORT,
    BQ_RESPONSE_BAD_CRC,
    /* INIT has bit 7 set: idle 0xFF bytes, not a response. */
    BQ_RESPONSE_NOT_A_RESPONSE,
} bq_response_status_t;

/* One response frame; data points into the caller's buffer. */
typedef struct {
    uint8_t        dev;
    uint16_t       reg;
    const uint8_t *data;
    size_t         len;
} bq_response_t;

/* Reads the response frame starting at buf[*pos] (len bytes in buf). On
   BQ_RESPONSE_OK fills out and moves *pos past the frame; otherwise leaves
   *pos alone, since nothing after a bad frame can be framed. */
bq_response_status_t bq_response_next(const uint8_t *buf, size_t len, size_t *pos,
                                      bq_response_t *out);

#endif /* BQ_FRAME_H */
