#ifndef APP_INC_CAN_IF_H
#define APP_INC_CAN_IF_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CAN_BUS_1 = 0,
    CAN_BUS_2 = 1,
    CAN_BUS_COUNT,
} can_bus_t;

/* One CAN frame, classic or FD. Deliberately compact (~72 B) rather
   than queueing FDCAN_RxHeaderTypeDef/FDCAN_TxHeaderTypeDef directly —
   those carry ~40 B of fields (timestamp, filter index, event-FIFO
   control...) this driver doesn't use yet, and every byte here is
   paid for twice, once per queue slot, across two buses. */
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
    bool     fd_capable;       /* this bus's compiled FrameFormat (Classic vs FD_BRS) */
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

/* Creates each bus's queues/mutex, configures the accept-all global
   filter (mandatory -- CubeMX never generates this call, and with zero
   filter slots configured the peripheral rejects every frame by
   default), starts both FDCAN peripherals, activates the RX FIFO0
   interrupt, and brings both STBY pins low (out of standby) per the
   ADM3057EBRWZ's confirmed polarity. Deliberately does NOT touch either
   termination GPIO -- termination only changes via an explicit
   can_set_termination() call, never as an init side effect. Must run
   before canRxTask/canTxTask are created — see tasks_init(). */
void can_if_init(void);

/* Queues one frame for canTxTask to send. Non-blocking (0 timeout);
   returns false without queuing if the hardware TX path can't accept
   it right now for a reason known immediately: the bus's compiled
   FrameFormat is Classic but is_fd is set, or len is out of range for
   the requested format. A false return from a full queue looks
   identical from the caller's side — check can_get_status()'s
   tx_drop_count to tell the two apart. */
bool can_send(can_bus_t bus, uint32_t id, const uint8_t *data, uint8_t len,
              bool is_extended, bool is_fd, bool is_brs);

/* Copies the most recently received frame on this bus into *out, but
   only if it's genuinely new since the last call with this *last_seq --
   pass a caller-owned uint32_t, zero-initialized, and this updates it
   in place each time it returns true. This is what `can stream` uses to
   print only on new arrivals rather than re-printing the same cached
   frame on a timer. It's still a cached single-frame snapshot, not the
   raw per-frame queue canRxTask itself drains (a second consumer of
   that queue would race canRxTask for frames and silently split
   traffic between them) -- if multiple frames arrive within one poll
   interval, only the latest of them is ever seen here. */
bool can_get_new_rx(can_bus_t bus, can_frame_t *out, uint32_t *last_seq);

bool can_get_status(can_bus_t bus, can_status_t *out);

/* Explicit manual overrides for testing the STBY and termination
   pins directly — separate from can_if_init()'s automatic
   out-of-standby-at-boot behavior. */
void can_set_standby(can_bus_t bus, bool standby);
void can_set_termination(can_bus_t bus, bool on);

/* Task entry points, each instantiated twice (once per bus) from
   tasks_init(). argument is the can_bus_t value itself, cast through
   (void *) — e.g. osThreadNew(canRxTask, (void *)CAN_BUS_1, ...) —
   not a struct pointer, so tasks.c never needs to see can_if.c's
   private per-bus context. RX and TX are necessarily separate tasks:
   they sit in different reserved priority bands
   (Documentation/tim-rtos-architecture.md section 2 — RX is "hard
   hardware deadlines", TX is "general control"), and a single task can
   only have one priority. */
void canRxTask(void *argument);
void canTxTask(void *argument);

#endif /* APP_INC_CAN_IF_H */