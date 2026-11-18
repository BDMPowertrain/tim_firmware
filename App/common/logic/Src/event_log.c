/* event_log.c — the Event log over the flash interface, see event_log.h. */

#include "event_log.h"

#include <string.h>

#include "crc16.h"

#define CRC_OFF 30u /* the CRC-16's offset, in a header and in an Event */

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

static uint16_t get_u16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)get_u16(b) | ((uint32_t)get_u16(&b[2]) << 16);
}

static uint32_t page_addr(uint32_t page)
{
    return EVENT_LOG_ADDR + page * FLASH_IF_PAGE_SIZE;
}

static uint32_t slot_addr(uint32_t page, uint32_t slot)
{
    return page_addr(page) + EVENT_SIZE + slot * EVENT_SIZE;
}

static void seal(uint8_t b[EVENT_SIZE])
{
    put_u16(&b[CRC_OFF], crc16(b, CRC_OFF));
}

static bool sealed(const uint8_t b[EVENT_SIZE])
{
    return get_u16(&b[CRC_OFF]) == crc16(b, CRC_OFF);
}

static bool blank(const uint8_t b[EVENT_SIZE])
{
    for (uint32_t i = 0; i < EVENT_SIZE; i++) {
        if (b[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

/* Written in part: a bad CRC with at least one double word still erased,
   as a write cut between two double words leaves it. A whole Event whose
   data happens to hold eight 0xFF bytes has a good CRC, so it is not
   taken for one. */
static bool partly_written(const uint8_t b[EVENT_SIZE])
{
    if (blank(b) || sealed(b)) {
        return false;
    }
    for (uint32_t off = 0; off < EVENT_SIZE; off += FLASH_IF_WORD_SIZE) {
        bool erased = true;

        for (uint32_t i = off; i < off + FLASH_IF_WORD_SIZE; i++) {
            erased = erased && b[i] == 0xFFu;
        }
        if (erased) {
            return true;
        }
    }
    return false;
}

void event_log_encode(const event_t *ev, uint8_t b[EVENT_SIZE])
{
    memset(b, 0, EVENT_SIZE);
    put_u16(&b[0], ev->boot);
    put_u16(&b[2], ev->code);
    put_u32(&b[4], ev->uptime_ms);
    put_u32(&b[8], ev->seq);
    memcpy(&b[12], ev->data, EVENT_DATA_SIZE);
    seal(b);
}

static void decode(const uint8_t b[EVENT_SIZE], event_t *ev)
{
    ev->boot = get_u16(&b[0]);
    ev->code = get_u16(&b[2]);
    ev->uptime_ms = get_u32(&b[4]);
    ev->seq = get_u32(&b[8]);
    memcpy(ev->data, &b[12], EVENT_DATA_SIZE);
}

/* Programs 32 bytes, one double word at a time. */
static bool write_32(const flash_if_t *flash, uint32_t addr, const uint8_t b[EVENT_SIZE])
{
    for (uint32_t off = 0; off < EVENT_SIZE; off += FLASH_IF_WORD_SIZE) {
        if (!flash->write(flash->ctx, addr + off, &b[off])) {
            return false;
        }
    }
    return true;
}

static bool write_header(const flash_if_t *flash, uint32_t page, uint32_t number)
{
    uint8_t b[EVENT_SIZE];

    memset(b, 0, sizeof b);
    put_u32(&b[0], EVENT_LOG_MARKER);
    put_u32(&b[4], number);
    seal(b);
    return write_32(flash, page_addr(page), b);
}

/* ---- the ring ---------------------------------------------------------- */

static bool empty(const event_log_t *log)
{
    return log->page_number[log->head] == 0u;
}

/* Erases the page after the newest and opens it as the new newest. */
static event_append_t open_next_page(event_log_t *log)
{
    const uint32_t number = empty(log) ? 1u : log->page_number[log->head] + 1u;
    const uint8_t page = empty(log) ? 0u : (uint8_t)((log->head + 1u) % EVENT_LOG_PAGES);

    log->page_number[page] = 0u;
    log->used[page] = 0u;
    if (!log->flash->erase(log->flash->ctx, page_addr(page))) {
        return EVENT_APPEND_ERASE_FAILED;
    }
    if (!write_header(log->flash, page, number)) {
        return EVENT_APPEND_WRITE_FAILED;
    }
    log->page_number[page] = number;
    log->head = page;
    return EVENT_APPEND_OK;
}

/* What a page's header says. */
typedef enum { PAGE_BLANK, PAGE_OURS, PAGE_BAD } page_kind_t;

static page_kind_t read_header(const flash_if_t *flash, uint32_t page, uint32_t *number)
{
    uint8_t b[EVENT_SIZE];

    if (!flash->read(flash->ctx, page_addr(page), b, sizeof b)) {
        return PAGE_BAD;
    }
    if (blank(b)) {
        return PAGE_BLANK;
    }
    *number = get_u32(&b[4]);
    if (!sealed(b) || get_u32(&b[0]) != EVENT_LOG_MARKER || *number == 0u) {
        return PAGE_BAD;
    }
    return PAGE_OURS;
}

/* Counts the slots written in an opened page: those up to the last one
   that is not blank. A slot that cannot be read, or that was written only
   in part, counts as written and sets *torn. */
static uint8_t count_used(const flash_if_t *flash, uint32_t page, bool *torn)
{
    uint8_t used = 0u;

    *torn = false;
    for (uint32_t slot = 0; slot < EVENT_LOG_SLOTS; slot++) {
        uint8_t b[EVENT_SIZE];

        if (!flash->read(flash->ctx, slot_addr(page, slot), b, sizeof b) || partly_written(b)) {
            *torn = true;
            used = (uint8_t)(slot + 1u);
        } else if (!blank(b)) {
            used = (uint8_t)(slot + 1u);
        }
    }
    return used;
}

static void record_repair(event_log_t *log, const event_log_repair_t *repair)
{
    if (log->repair_count < EVENT_LOG_REPAIRS_MAX) {
        log->repair[log->repair_count++] = *repair;
    }
    log->repairs++;
}

/* A header that cannot be read or is not ours: nothing in the page can be
   trusted, so it is erased and left blank. */
static void repair_header(event_log_t *log, uint32_t page)
{
    const event_log_repair_t repair = {
        .page = (uint8_t)page,
        .header = true,
        .erased = log->flash->erase(log->flash->ctx, page_addr(page)),
    };

    record_repair(log, &repair);
}

/* An opened page holding a word that cannot be read, or an Event written
   in part: its whole Events are
   kept in RAM, the page is erased again and they are written back in
   order under the same header (ES0430 2.2.4). Sets the page's number and
   used count to what it holds after. */
static void repair_page(event_log_t *log, uint32_t page, uint32_t number, uint8_t used)
{
    const flash_if_t *flash = log->flash;
    event_log_repair_t repair = { .page = (uint8_t)page };

    for (uint32_t slot = 0; slot < used; slot++) {
        uint8_t *b = &log->page_buf[repair.kept * EVENT_SIZE];

        if (flash->read(flash->ctx, slot_addr(page, slot), b, EVENT_SIZE) && sealed(b) && !blank(b)) {
            repair.kept++;
        }
    }

    repair.erased = flash->erase(flash->ctx, page_addr(page));
    if (!repair.erased) {
        /* Left as found: the reader skips what it cannot read. */
        log->page_number[page] = number;
        log->used[page] = used;
        record_repair(log, &repair);
        return;
    }

    log->page_number[page] = 0u;
    log->used[page] = 0u;
    if (write_header(flash, page, number)) {
        log->page_number[page] = number;
        for (uint32_t slot = 0; slot < repair.kept; slot++) {
            log->used[page]++;
            if (!write_32(flash, slot_addr(page, slot), &log->page_buf[slot * EVENT_SIZE])) {
                break;
            }
        }
    }
    record_repair(log, &repair);
}

/* The newest Event that reads whole gives the last start's boot number. */
static uint16_t recover_boot(const event_log_t *log)
{
    event_pos_t pos;
    bool more = event_log_newest(log, &pos);

    while (more) {
        event_t ev;

        if (event_log_read(log, pos, &ev) == EVENT_READ_OK) {
            return (uint16_t)(ev.boot + 1u);
        }
        more = event_log_older(log, &pos);
    }
    return 1u;
}

void event_log_open(event_log_t *log, const flash_if_t *flash)
{
    memset(log, 0, sizeof *log);
    log->flash = flash;
    log->boot = 1u;
    if (flash == NULL) {
        return;
    }

    for (uint32_t page = 0; page < EVENT_LOG_PAGES; page++) {
        uint32_t number = 0u;
        const page_kind_t kind = read_header(flash, page, &number);

        if (kind == PAGE_BLANK) {
            continue;
        }
        if (kind == PAGE_BAD) {
            repair_header(log, page);
            continue;
        }

        bool torn;
        const uint8_t used = count_used(flash, page, &torn);

        if (torn) {
            repair_page(log, page, number, used);
        } else {
            log->page_number[page] = number;
            log->used[page] = used;
        }
        if (log->page_number[page] > log->page_number[log->head]) {
            log->head = (uint8_t)page;
        }
    }
    log->boot = recover_boot(log);
}

event_append_t event_log_append(event_log_t *log, uint16_t code, uint32_t uptime_ms,
                                const uint8_t data[EVENT_DATA_SIZE])
{
    event_t ev = {
        .boot = log->boot,
        .code = code,
        .uptime_ms = uptime_ms,
        .seq = log->seq++,
    };

    if (log->flash == NULL) {
        return EVENT_APPEND_NO_FLASH;
    }
    memcpy(ev.data, data, EVENT_DATA_SIZE);

    if (empty(log) || log->used[log->head] >= EVENT_LOG_SLOTS) {
        const event_append_t opened = open_next_page(log);

        if (opened != EVENT_APPEND_OK) {
            log->write_failures++;
            return opened;
        }
    }

    uint8_t b[EVENT_SIZE];

    event_log_encode(&ev, b);
    /* The slot is used whether or not the write works: a half-written one
       is never written again. */
    const uint8_t slot = log->used[log->head]++;

    if (!write_32(log->flash, slot_addr(log->head, slot), b)) {
        log->write_failures++;
        return EVENT_APPEND_WRITE_FAILED;
    }
    return EVENT_APPEND_OK;
}

/* ---- reading ----------------------------------------------------------- */

bool event_log_newest(const event_log_t *log, event_pos_t *pos)
{
    if (empty(log)) {
        return false;
    }

    /* The newest page may hold nothing yet: then the newest slot is the
       last of the next page back that holds one. */
    pos->page = log->head;
    pos->slot = 0u;
    if (log->used[log->head] == 0u) {
        return event_log_older(log, pos);
    }
    pos->slot = (uint8_t)(log->used[log->head] - 1u);
    return true;
}

/* The page next in ring order from page, older or newer, holding at least
   one slot; EVENT_LOG_PAGES if none. */
static uint32_t next_page(const event_log_t *log, uint32_t page, bool older)
{
    const uint32_t from = log->page_number[page];
    uint32_t best = EVENT_LOG_PAGES;

    for (uint32_t p = 0; p < EVENT_LOG_PAGES; p++) {
        const uint32_t n = log->page_number[p];

        if (n == 0u || log->used[p] == 0u || (older ? n >= from : n <= from)) {
            continue;
        }
        if (best == EVENT_LOG_PAGES
            || (older ? n > log->page_number[best] : n < log->page_number[best])) {
            best = p;
        }
    }
    return best;
}

bool event_log_older(const event_log_t *log, event_pos_t *pos)
{
    if (pos->slot > 0u) {
        pos->slot--;
        return true;
    }

    const uint32_t page = next_page(log, pos->page, true);

    if (page == EVENT_LOG_PAGES) {
        return false;
    }
    pos->page = (uint8_t)page;
    pos->slot = (uint8_t)(log->used[page] - 1u);
    return true;
}

bool event_log_newer(const event_log_t *log, event_pos_t *pos)
{
    if (pos->slot + 1u < log->used[pos->page]) {
        pos->slot++;
        return true;
    }

    const uint32_t page = next_page(log, pos->page, false);

    if (page == EVENT_LOG_PAGES) {
        return false;
    }
    pos->page = (uint8_t)page;
    pos->slot = 0u;
    return true;
}

event_read_t event_log_read(const event_log_t *log, event_pos_t pos, event_t *out)
{
    uint8_t b[EVENT_SIZE];

    if (log->flash == NULL || pos.page >= EVENT_LOG_PAGES || pos.slot >= log->used[pos.page]
        || log->page_number[pos.page] == 0u) {
        return EVENT_READ_BLANK;
    }
    if (!log->flash->read(log->flash->ctx, slot_addr(pos.page, pos.slot), b, sizeof b)) {
        return EVENT_READ_FAULT;
    }
    if (blank(b)) {
        return EVENT_READ_BLANK;
    }
    if (!sealed(b)) {
        return EVENT_READ_BAD_CRC;
    }
    decode(b, out);
    return EVENT_READ_OK;
}

bool event_log_find(const event_log_t *log, bool (*match)(const event_t *ev, const void *ctx),
                    const void *ctx, event_t *out)
{
    event_pos_t pos;
    bool more = event_log_newest(log, &pos);

    while (more) {
        if (event_log_read(log, pos, out) == EVENT_READ_OK && match(out, ctx)) {
            return true;
        }
        more = event_log_older(log, &pos);
    }
    return false;
}
