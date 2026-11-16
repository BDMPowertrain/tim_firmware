#include "mailbox.h"

#include <string.h>

/*
 * How long after `since_ms` `at_ms` is. A time a little before `since_ms`
 * gives zero, not four billion: see mailbox_update() in the header.
 */
static uint32_t elapsed_ms(uint32_t at_ms, uint32_t since_ms)
{
    const uint32_t elapsed = at_ms - since_ms;

    return (elapsed > (uint32_t)INT32_MAX) ? 0u : elapsed;
}

/* Forget the run of good frames and the sender's counter. */
static void start_again(mailbox_t *mb)
{
    mb->good_run = 0;
    mb->e2e = (e2e_p11_rx_t){0};
}

/*
 * If the last good frame is the timeout old at `at_ms`, the run of good
 * frames is over and a VALID mailbox is INVALID.
 */
static void age(mailbox_t *mb, const mailbox_config_t *config, uint32_t at_ms)
{
    if (mb->good_run == 0u ||
        elapsed_ms(at_ms, mb->good_arrival_ms) < config->timeout_ms) {
        return;
    }

    if (mb->state == MAILBOX_VALID) {
        mb->state = MAILBOX_INVALID;
        mb->counters.timeouts++;
    }
    start_again(mb);
}

/* Checks a frame and counts it. True if it is good. */
static bool check(mailbox_t *mb, const mailbox_config_t *config,
                  const mailbox_rx_t *frame)
{
    if (frame->len < config->length) {
        mb->counters.bad_length++;
        return false;
    }

    if (config->e2e) {
        switch (e2e_p11_check(&mb->e2e, config->e2e_data_id, frame->data,
                              config->length)) {
        case E2E_P11_OK:
            break;
        case E2E_P11_BAD_CRC:
            mb->counters.bad_crc++;
            return false;
        case E2E_P11_REPEATED:
        case E2E_P11_SKIPPED:
        default:
            mb->counters.bad_counter++;
            return false;
        }
    }

    mb->counters.good++;
    return true;
}

static void take_new_frame(mailbox_t *mb, const mailbox_config_t *config,
                           const mailbox_rx_t *frame)
{
    /* The gap before this frame is judged first, so a step that ran late
     * cannot join two frames that were a timeout apart. */
    age(mb, config, frame->arrival_ms);

    if (!check(mb, config, frame)) {
        /* Dropped. A VALID mailbox stays VALID until its timeout. The
         * sender's counter is kept: the E2E check has already moved on. */
        if (mb->state != MAILBOX_VALID) {
            mb->good_run = 0;
        }
        return;
    }

    memcpy(mb->data, frame->data, sizeof mb->data);
    mb->good_arrival_ms = frame->arrival_ms;
    if (mb->good_run < MAILBOX_GOOD_FRAMES_FOR_VALID) {
        mb->good_run++;
    }
    if (mb->good_run >= MAILBOX_GOOD_FRAMES_FOR_VALID) {
        mb->state = MAILBOX_VALID;
    }
}

void mailbox_rx_store(mailbox_rx_t *rx, const uint8_t *data, uint8_t len,
                      uint32_t now_ms)
{
    if (len > MAILBOX_FRAME_MAX) {
        len = MAILBOX_FRAME_MAX;
    }
    memcpy(rx->data, data, len);
    rx->len = len;
    rx->arrival_ms = now_ms;
    rx->count++;
}

mailbox_state_t mailbox_update(mailbox_t *mb, const mailbox_config_t *config,
                               const mailbox_rx_t *snapshot, uint32_t now_ms)
{
    const uint32_t new_frames = snapshot->count - mb->seen_count;

    if (new_frames != 0u) {
        mb->seen_count = snapshot->count;
        mb->counters.overwritten += new_frames - 1u;
        take_new_frame(mb, config, snapshot);
    }
    age(mb, config, now_ms);

    return mb->state;
}

const uint8_t *mailbox_read(const mailbox_t *mb, const mailbox_config_t *config)
{
    return (mb->state == MAILBOX_VALID) ? mb->data : config->substitute;
}

void mailbox_force_invalid(mailbox_t *mb)
{
    mb->state = MAILBOX_INVALID;
    start_again(mb);
}

void mailbox_force_all_invalid(mailbox_t *mailboxes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        mailbox_force_invalid(&mailboxes[i]);
    }
}
