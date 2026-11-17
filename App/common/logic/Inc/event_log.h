/*
 * The Event log (ADR 0014, spec #36): what happened and when, in bank 2,
 * kept across reset, power-off and a normal flash of the image, with no SD
 * card involved.
 *
 * A ring of EVENT_LOG_PAGES pages of FLASH_IF_PAGE_SIZE bytes from
 * EVENT_LOG_ADDR. Each page starts with a header, then holds
 * EVENT_LOG_SLOTS Events written in order, each EVENT_SIZE bytes,
 * little-endian:
 *
 *   header: 0 marker EVENT_LOG_MARKER, 4 page number, 8..29 zero,
 *           30 CRC-16 of bytes 0 to 29
 *   Event:  0 boot number (u16), 2 event code (u16), 4 uptime ms (u32),
 *           8 sequence number (u32), 12 data (16 bytes), 28 zero (u16),
 *           30 CRC-16 of bytes 0 to 29
 *
 * The page number goes up by one for each page opened, so the ring's order
 * is the order of page numbers, whatever the page's place in bank 2. When
 * the newest page is full the next page in bank 2, the oldest or a blank
 * one, is erased and opened with its header. A page with no header is
 * blank and holds nothing.
 *
 * The boot number is one more than the newest Event's found at open (1 on
 * an empty log); the sequence number counts from 0 at each open and goes
 * up for every Event appended, written or not, so a gap shows a loss.
 *
 * A reset or power loss during a write leaves a double word undefined, and
 * reading it fails (flash_if.h). Open reads every page: a page whose header
 * cannot be read or is not ours is erased, and a page holding a word that
 * cannot be read is repaired: its good Events are kept in RAM, the page is
 * erased again (ES0430 2.2.4) and they are written back in order under the
 * same header. A loss of power during a repair loses that page's Events.
 * An Event written only in part (a write cut between two of its
 * double words: some programmed, one still erased) is half-written too, and
 * its page is repaired the same way. Each repair is kept for the caller to
 * report as an Event; the first EVENT_LOG_REPAIRS_MAX of an open are kept,
 * every one is counted. A whole Event whose CRC is wrong (corruption) is
 * skipped by the reader and repairs nothing.
 *
 * Without flash (the part is not dual-bank) nothing is read or written.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "flash_if.h"

#define EVENT_LOG_ADDR   0x08042000u
#define EVENT_LOG_PAGES  32u
#define EVENT_SIZE       32u
#define EVENT_DATA_SIZE  16u
#define EVENT_LOG_SLOTS  ((FLASH_IF_PAGE_SIZE - EVENT_SIZE) / EVENT_SIZE) /* 63 */
#define EVENT_LOG_MARKER 0x4C545645u /* "EVTL" */

/* Repairs kept from one open for the caller to report. */
#define EVENT_LOG_REPAIRS_MAX 4u

typedef struct {
    uint16_t boot;
    uint16_t code;
    uint32_t uptime_ms;
    uint32_t seq;
    uint8_t  data[EVENT_DATA_SIZE];
} event_t;

/* One page repaired at open. */
typedef struct {
    uint8_t page;   /* its place in bank 2, 0 to EVENT_LOG_PAGES - 1 */
    uint8_t kept;   /* good Events written back */
    bool    header; /* the header itself was unreadable or not ours */
    bool    erased; /* the erase worked; false leaves the page as found */
} event_log_repair_t;

typedef struct {
    const flash_if_t *flash; /* NULL: no flash */

    uint32_t page_number[EVENT_LOG_PAGES]; /* 0: blank */
    uint8_t  used[EVENT_LOG_PAGES];        /* slots written, good or not */
    uint8_t  head;                         /* the newest page, if any */

    uint16_t boot; /* this start's */
    uint32_t seq;  /* the next Event's */

    event_log_repair_t repair[EVENT_LOG_REPAIRS_MAX];
    uint8_t  repair_count; /* in repair[], at most EVENT_LOG_REPAIRS_MAX */
    uint32_t repairs;      /* pages repaired at open, all counted */
    uint32_t write_failures;

    uint8_t  page_buf[FLASH_IF_PAGE_SIZE]; /* a repair's good Events */
} event_log_t;

typedef enum {
    EVENT_APPEND_OK,
    EVENT_APPEND_NO_FLASH,
    EVENT_APPEND_ERASE_FAILED, /* opening the next page */
    EVENT_APPEND_WRITE_FAILED,
} event_append_t;

/* Where one Event is: a page's place in bank 2 and the slot in it. */
typedef struct {
    uint8_t page;
    uint8_t slot;
} event_pos_t;

typedef enum {
    EVENT_READ_OK,
    EVENT_READ_BAD_CRC, /* written, but not whole: skip it */
    EVENT_READ_FAULT,   /* the read failed: a torn word */
    EVENT_READ_BLANK,   /* nothing written there (now): a reader racing the
                           writer has walked onto a page it erased */
} event_read_t;

/* Scans the ring, repairs what needs it and recovers the boot number.
   flash is NULL when the part is not dual-bank. */
void event_log_open(event_log_t *log, const flash_if_t *flash);

/* Writes one Event with this start's boot number and the next sequence
   number, opening the next page first if the newest is full. */
event_append_t event_log_append(event_log_t *log, uint16_t code, uint32_t uptime_ms,
                                const uint8_t data[EVENT_DATA_SIZE]);

/* The newest slot written. False if the log is empty. */
bool event_log_newest(const event_log_t *log, event_pos_t *pos);

/* Moves pos one slot older or newer, across pages in ring order. False,
   with pos unchanged, at the oldest or newest slot. */
bool event_log_older(const event_log_t *log, event_pos_t *pos);
bool event_log_newer(const event_log_t *log, event_pos_t *pos);

/* Reads the Event at pos. */
event_read_t event_log_read(const event_log_t *log, event_pos_t pos, event_t *out);

/* Walks from the newest Event to the oldest for the first one match()
   accepts, skipping any that does not read whole. True, with *out that
   Event, if one does. Nothing is found in an empty log or without flash. */
bool event_log_find(const event_log_t *log, bool (*match)(const event_t *ev, const void *ctx),
                    const void *ctx, event_t *out);

/* The 32 bytes the log stores for ev, its CRC included, laid out as at the
   top of this file. The Data log copies each Event in this form
   (data_log_record.h). */
void event_log_encode(const event_t *ev, uint8_t out[EVENT_SIZE]);

#endif /* EVENT_LOG_H */
