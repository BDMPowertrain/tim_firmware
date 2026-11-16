/*
 * Mailbox (ADR 0004): the latest copy of one received Control bus frame,
 * with how old it is and whether it can be trusted. A newer frame replaces
 * the older one; nothing queues.
 *
 * A mailbox has two halves, because two contexts write it:
 *   mailbox_rx_t  the latest raw frame and its arrival time, written by the
 *                 receive interrupt with mailbox_rx_store() and nothing else;
 *   mailbox_t     the state, the last good frame and the counters, owned by
 *                 the control step.
 * At the start of a step the control step copies each mailbox_rx_t it uses
 * (the snapshot) and hands the copy to mailbox_update() with the time. All
 * checking and ageing happens there, never in the interrupt.
 *
 * Rules:
 * - A frame is good if it is at least as long as the Message database says
 *   and, on our own frames, passes the E2E Profile 11 CRC and counter check.
 *   Any other frame is dropped and counted, as if it had not been received.
 * - VALID needs MAILBOX_GOOD_FRAMES_FOR_VALID good frames in a row, from
 *   NODATA and from INVALID. A dropped frame, or a gap of the timeout or
 *   more between two good frames, starts the run again.
 * - VALID becomes INVALID only when the last good frame is timeout_ms old or
 *   older (an age of exactly timeout_ms has timed out), or when the mailbox
 *   is forced INVALID for a bus-off. A dropped frame never does it.
 * - A reader of a mailbox that is not VALID gets the frame's Substitute,
 *   never the last real value.
 *
 * Time is a 32-bit millisecond count compared only by subtraction, so the
 * wrap is harmless. Call mailbox_update() every step: it must run more often
 * than once in 2^31 ms.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef MAILBOX_H
#define MAILBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "e2e_p11.h"

#define MAILBOX_FRAME_MAX             8u /* the Control bus is classic CAN */
#define MAILBOX_GOOD_FRAMES_FOR_VALID 3u

typedef enum {
    MAILBOX_NODATA,  /* never VALID since the last reset */
    MAILBOX_VALID,   /* fresh and passing its checks */
    MAILBOX_INVALID  /* timed out, or forced for a bus-off */
} mailbox_state_t;

/* What is fixed about one received frame. One table row per frame. */
typedef struct {
    uint32_t frame_id;    /* CAN ID */
    bool     extended;    /* 29-bit ID */
    uint8_t  length;      /* bytes; a shorter frame is dropped */
    bool     e2e;         /* carries the E2E Profile 11 CRC and counter */
    uint16_t e2e_data_id; /* its Data ID, if so */
    uint32_t timeout_ms;  /* a good frame this old or older has timed out */
    uint8_t  substitute[MAILBOX_FRAME_MAX]; /* the Substitute, as frame bytes */
} mailbox_config_t;

/* The interrupt's half. Zero-initialise. */
typedef struct {
    uint8_t  data[MAILBOX_FRAME_MAX];
    uint8_t  len;        /* bytes received */
    uint32_t arrival_ms; /* when it arrived */
    uint32_t count;      /* frames stored since the last reset; marks a new frame */
} mailbox_rx_t;

typedef struct {
    uint32_t good;        /* frames accepted */
    uint32_t bad_length;  /* dropped: shorter than the Message database says */
    uint32_t bad_crc;     /* dropped: E2E CRC mismatch */
    uint32_t bad_counter; /* dropped: E2E counter repeated or skipped */
    uint32_t overwritten; /* replaced by a newer frame before a step saw them */
    uint32_t timeouts;    /* VALID to INVALID through the timeout */
} mailbox_counters_t;

/* The control step's half. Zero-initialise: that is NODATA. */
typedef struct {
    mailbox_state_t    state;
    uint8_t            good_run;        /* good frames in a row, up to 3 */
    uint8_t            data[MAILBOX_FRAME_MAX]; /* the last good frame */
    uint32_t           good_arrival_ms; /* its arrival; meaningful if good_run > 0 */
    uint32_t           seen_count;      /* rx.count at the last update */
    e2e_p11_rx_t       e2e;
    mailbox_counters_t counters;
} mailbox_t;

/*
 * The receive interrupt's whole job: replace the frame in the mailbox.
 * len beyond MAILBOX_FRAME_MAX is cut to it.
 */
void mailbox_rx_store(mailbox_rx_t *rx, const uint8_t *data, uint8_t len,
                      uint32_t now_ms);

/*
 * Run once per control step on the snapshot of the interrupt's half. Checks
 * the frame if it is new, ages the mailbox against now_ms and returns the
 * state. An arrival time a little after now_ms (the interrupt ran between
 * reading the clock and taking the snapshot) is an age of zero.
 */
mailbox_state_t mailbox_update(mailbox_t *mb, const mailbox_config_t *config,
                               const mailbox_rx_t *snapshot, uint32_t now_ms);

/*
 * The frame a reader may use, config->length bytes, ready for the frame's
 * generated unpack function: the last good frame if the mailbox is VALID,
 * the Substitute otherwise.
 */
const uint8_t *mailbox_read(const mailbox_t *mb, const mailbox_config_t *config);

/*
 * Bus-off: INVALID now, whatever the state was, and VALID again only after
 * three new good frames. Not counted as a timeout.
 */
void mailbox_force_invalid(mailbox_t *mb);
void mailbox_force_all_invalid(mailbox_t *mailboxes, size_t count);

#endif /* MAILBOX_H */
