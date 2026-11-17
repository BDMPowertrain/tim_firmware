/*
 * The Data log's write batch (ADR 0021, issue 53): encoded Records collect
 * in a 4 KB buffer, and the logger hands the card whole 512-byte sectors
 * only, so FatFS writes them straight to the card with no read-modify-write
 * of a part sector. The bytes after the last whole sector wait for more;
 * only closing a file writes them.
 *
 * A Record may be split across two writes: the file is a byte stream.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef LOG_BATCH_H
#define LOG_BATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOG_BATCH_SIZE   4096u
#define LOG_BATCH_SECTOR 512u

typedef struct {
    uint8_t buf[LOG_BATCH_SIZE];
    size_t  used;
} log_batch_t;

/* Empty. */
void log_batch_init(log_batch_t *b);

/* Appends as much of data as fits; returns the bytes taken. */
size_t log_batch_put(log_batch_t *b, const uint8_t *data, size_t len);

/* No room for another byte: write the sectors before the next put. */
bool log_batch_full(const log_batch_t *b);

/* The bytes waiting, from log_batch_data(). */
size_t log_batch_used(const log_batch_t *b);

/* The bytes in whole sectors at the front, from log_batch_data(). */
size_t log_batch_sectors(const log_batch_t *b);

const uint8_t *log_batch_data(const log_batch_t *b);

/* Drops the first n bytes, now written, and moves the rest to the front. */
void log_batch_consume(log_batch_t *b, size_t n);

#endif /* LOG_BATCH_H */
