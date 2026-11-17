/* log_batch.c — the Data log's write batch, see log_batch.h. */

#include "log_batch.h"

#include <string.h>

void log_batch_init(log_batch_t *b)
{
    b->used = 0u;
}

size_t log_batch_put(log_batch_t *b, const uint8_t *data, size_t len)
{
    const size_t room = LOG_BATCH_SIZE - b->used;
    const size_t n = len < room ? len : room;

    if (n > 0u) {
        memcpy(&b->buf[b->used], data, n);
        b->used += n;
    }
    return n;
}

bool log_batch_full(const log_batch_t *b)
{
    return b->used >= LOG_BATCH_SIZE;
}

size_t log_batch_used(const log_batch_t *b)
{
    return b->used;
}

size_t log_batch_sectors(const log_batch_t *b)
{
    return b->used - (b->used % LOG_BATCH_SECTOR);
}

const uint8_t *log_batch_data(const log_batch_t *b)
{
    return b->buf;
}

void log_batch_consume(log_batch_t *b, size_t n)
{
    if (n >= b->used) {
        b->used = 0u;
        return;
    }
    memmove(b->buf, &b->buf[n], b->used - n);
    b->used -= n;
}
