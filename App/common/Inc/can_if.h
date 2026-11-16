#ifndef APP_INC_CAN_IF_H
#define APP_INC_CAN_IF_H

/* can_if — the DAQ bus (CAN2) driver: CAN FD, logging and display data,
   nothing safety-related (GLOSSARY.md). The Control bus (CAN1) has its
   own driver, control_bus.h, used only by the Control step (ADR 0004). */

#include <stdbool.h>
#include <stdint.h>

/* One CAN frame, classic or FD. Deliberately compact (~72 B) rather
   than queueing FDCAN_RxHeaderTypeDef/FDCAN_TxHeaderTypeDef directly —
   those carry ~40 B of fields (timestamp, filter index, event-FIFO
   control...) this driver doesn't use yet, and every byte here is
   paid for twice, once per queue slot. */
typedef struct {
    uint32_t id;
    uint8_t  len;          /* real byte count: 0-8 classic; 12/16/20/24/32/48/64 for FD */
    bool     is_extended;
    bool     is_fd;
    bool     is_brs;       /* bit-rate switching; only meaningful if is_fd */
    uint8_t  data[64];
} can_frame_t;

typedef struct {
    bool     standby;          /* last commanded STBY state */
    bool     terminated;       /* last commanded termination-relay state --
                                   NOT a measured fact, see can_set_termination() */
    bool     fd_capable;       /* the bus's compiled FrameFormat (Classic vs FD_BRS) */
    bool     bus_off;
    bool     error_passive;
    uint32_t last_error_code;  /* FDCAN_LastErrorCode value, see HAL header */
    uint32_t tx_error_count;
    uint32_t rx_error_count;
    uint32_t rx_count;         /* frames successfully queued to canRxTask */
    uint32_t tx_count;         /* frames successfully handed to the TX hardware FIFO */
    uint32_t rx_drop_count;    /* ISR->queue drops (rxQueue full) */
    uint32_t tx_drop_count;    /* TX hardware FIFO full, or rejected (FD frame on a
                                   classic-compiled bus) */
} can_status_t;

/* Creates the bus's queues and mutex, configures the accept-all global
   filter (mandatory -- CubeMX never generates this call, and with zero
   filter slots configured the peripheral rejects every frame by
   default), starts FDCAN2, activates the RX FIFO0 interrupt, and brings
   STBY low (out of standby) per the ADM3057EBRWZ's confirmed polarity.
   Deliberately does NOT touch the termination GPIO -- termination only
   changes via an explicit can_set_termination() call, never as an init
   side effect. Must run before canRxTask/canTxTask are created — see
   tasks_init(). Returns false if a queue, the mutex or the controller
   could not be set up. */
bool can_if_init(void);

/* Queues one frame for canTxTask to send. Non-blocking (0 timeout);
   returns false without queuing if the hardware TX path can't accept
   it right now for a reason known immediately: the bus's compiled
   FrameFormat is Classic but is_fd is set, or len is out of range for
   the requested format. A false return from a full queue looks
   identical from the caller's side — check can_get_status()'s
   tx_drop_count to tell the two apart. */
bool can_send(uint32_t id, const uint8_t *data, uint8_t len,
              bool is_extended, bool is_fd, bool is_brs);

/* Copies the most recently received frame into *out, but only if it's
   genuinely new since the last call with this *last_seq -- pass a
   caller-owned uint32_t, zero-initialized, and this updates it in place
   each time it returns true. This is what `can stream2` uses to print
   only on new arrivals rather than re-printing the same cached frame on
   a timer. It's still a cached single-frame snapshot, not the raw
   per-frame queue canRxTask itself drains (a second consumer of that
   queue would race canRxTask for frames and silently split traffic
   between them) -- if multiple frames arrive within one poll interval,
   only the latest of them is ever seen here. */
bool can_get_new_rx(can_frame_t *out, uint32_t *last_seq);

bool can_get_status(can_status_t *out);

/* Explicit manual overrides for testing the STBY and termination pins
   directly — separate from can_if_init()'s automatic out-of-standby-at-
   boot behavior. */
void can_set_standby(bool standby);
void can_set_termination(bool on);

/* Task entry points, created once each from tasks_init(). RX and TX are
   separate tasks so that a slow sender never delays draining the
   receive queue. */
void canRxTask(void *argument);
void canTxTask(void *argument);

#endif /* APP_INC_CAN_IF_H */
