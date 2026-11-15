/*
 * AUTOSAR E2E Profile 11: the CRC and counter carried by the safety-relevant
 * frames we own on the Control bus (BMS_Status, BMS_Limits, BMS_Power,
 * VCU_Status).
 *
 * Layout, in every protected frame:
 *   byte 0          CRC-8 SAE J1850 (polynomial 0x1D)
 *   byte 1, bits 0-3  counter, 0 to 14, then back to 0 (15 is never sent)
 *   byte 1, bits 4-7  free for a signal; covered by the CRC
 *   bytes 2 onward  signals; covered by the CRC
 *
 * Each frame also has a 16-bit Data ID that is not sent but is part of the
 * CRC (the profile's "both bytes" Data ID mode), so a frame arriving under
 * the wrong CAN ID fails the CRC.
 *
 * This is logic (ADR 0002): no HAL, no RTOS, no clock.
 */
#ifndef E2E_P11_H
#define E2E_P11_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E2E_P11_MIN_LEN     2u   /* the CRC byte and the counter byte */
#define E2E_P11_COUNTER_MAX 14u

/* Sender state for one frame. Zero-initialise: the first frame carries 0. */
typedef struct {
    uint8_t counter; /* the counter the next frame will carry */
} e2e_p11_tx_t;

/* Receiver state for one frame. Zero-initialise. */
typedef struct {
    bool    seen;         /* a frame with a good CRC has been received */
    uint8_t last_counter; /* its counter */
} e2e_p11_rx_t;

typedef enum {
    E2E_P11_OK,       /* good CRC, counter one past the last (or first frame) */
    E2E_P11_BAD_CRC,  /* CRC mismatch or frame too short; counter not looked at */
    E2E_P11_REPEATED, /* good CRC, same counter as the last frame */
    E2E_P11_SKIPPED   /* good CRC, counter not one past the last, or 15 */
} e2e_p11_result_t;

/*
 * Writes the counter and the CRC into frame[0] and frame[1] (the upper half
 * of frame[1] is kept) and advances the counter. Fill in the signals first.
 * len is the frame length in bytes, at least E2E_P11_MIN_LEN; a shorter
 * frame is left untouched.
 */
void e2e_p11_protect(e2e_p11_tx_t *tx, uint16_t data_id, uint8_t *frame,
                     size_t len);

/*
 * Checks a received frame. Anything but E2E_P11_OK means the frame must not
 * be used. A frame with a good CRC and a counter from 0 to 14 becomes the new
 * reference, so the frame after a skip is OK again.
 * len is the frame length in bytes as received; a frame shorter than
 * E2E_P11_MIN_LEN is E2E_P11_BAD_CRC.
 */
e2e_p11_result_t e2e_p11_check(e2e_p11_rx_t *rx, uint16_t data_id,
                               const uint8_t *frame, size_t len);

#endif /* E2E_P11_H */
