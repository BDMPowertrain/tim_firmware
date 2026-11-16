#ifndef APP_INC_CONTROL_BUS_H
#define APP_INC_CONTROL_BUS_H

/* control_bus — the Control bus (CAN1) driver, used by the Control step
   and by nothing else (ADR 0004, ADR 0010).

   Receiving: the acceptance filters pass only the frames the image reads
   and reject every other ID. The receive interrupt writes each frame and
   its arrival time into that frame's mailbox (mailbox_rx_store) and does
   nothing else. At the start of each step, control_bus_update() copies
   every mailbox's interrupt half with interrupts masked and runs the
   mailbox logic on the copy.

   Sending: no software queue. The step builds a frame when it is due and
   control_bus_send() hands it straight to one of the controller's three
   send slots, which go out in ID-priority order. No free slot: the frame
   is skipped and counted.

   CAN1 is classic CAN at 500 kbit/s; its filters, frame format and send
   mode are in the image's tim.ioc. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "control_rx.h"
#include "mailbox.h"

/* The most frames one image reads: the VCU's ten standard and one
   extended (control_rx.h). */
#define CONTROL_BUS_RX_MAX 11u

typedef enum {
    CONTROL_BUS_FIFO0,
    CONTROL_BUS_FIFO1,
} control_bus_fifo_t;

/* One frame an image reads, and the receive FIFO its filter sends it to. */
typedef struct {
    control_rx_t       frame;
    control_bus_fifo_t fifo;
} control_bus_rx_t;

typedef struct {
    bool     started;
    bool     bus_off;
    bool     error_passive;
    uint32_t last_error_code; /* FDCAN_LastErrorCode value, see HAL header */
    uint32_t tx_error_count;
    uint32_t rx_error_count;
    uint32_t rx_frames;   /* frames written into a mailbox */
    uint32_t rx_unknown;  /* frames past the filters with no mailbox: 0 */
    uint32_t tx_sent;     /* frames handed to a send slot */
    uint32_t tx_skipped;  /* frames skipped: no free slot, or not started */
} control_bus_status_t;

/* Sets the acceptance filters for rows (the image's received frames, one
   mailbox each, in this order), arms the receive interrupts and takes the
   transceiver out of standby. Termination is not touched: it stays open
   until it is set from a parameter. Does not start CAN1; the first
   Control step does, at least one period later. Call once from
   tasks_init(). Returns false if the filters do not fit the slots tim.ioc
   gives or the controller refuses them. */
bool control_bus_init(const control_bus_rx_t *rows, size_t row_count);

/* Starts CAN1. Called by the first Control step. */
bool control_bus_start(void);

/* The start of each step: snapshot every mailbox and update it against
   now_ms. Only the Control step calls this. */
void control_bus_update(uint32_t now_ms);

/* The mailbox of rows[row] after the last update. Only the Control step
   reads it. */
const mailbox_t *control_bus_mailbox(size_t row);

/* Hands one classic frame of up to 8 bytes to a free send slot. False if
   none is free or CAN1 has not started; that is counted as skipped. */
bool control_bus_send(uint32_t frame_id, bool extended, const uint8_t *data,
                      uint8_t len);

/* For the shell: counters and the controller's error state. */
bool control_bus_get_status(control_bus_status_t *out);

#endif /* APP_INC_CONTROL_BUS_H */
