#ifndef APP_INC_CAN_IF_H
#define APP_INC_CAN_IF_H

/* can_if — the DAQ bus (CAN2) driver: CAN FD, 500 kbit/s arbitration and
   2 Mbit/s data, logging and display data, nothing safety-related
   (GLOSSARY.md). The Control bus (CAN1) has its own driver, control_bus.h,
   used only by the Control step (ADR 0004).

   Isolation from the Control bus (issue 36, "DAQ bus isolation"):
   - one Normal-priority task, daqTask, owns sending. can_send() copies the
     frame into a send queue (frame_queue.h) in a short critical section
     and returns: it never waits. A full queue drops its oldest frame.
   - the DAQ publisher runs inside daqTask every 100 ms. It copies the
     Control step's published snapshot (control_step_read()) and sends the
     image's status frame, then the image's own frames, which the image
     builds from its own copy of the snapshot (the BMS: its module
     summaries and cell dump). The step itself never calls anything here.
   - CAN2's interrupts sit at priority 7, below CAN1's 5 (tim.ioc).
   - a bus-off is retried once per second forever (daq_retry.h) and
     affects nothing else. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can_frame.h"
#include "daq_status.h"
#include "frame_queue.h"

/* The most frames build_frames may give at one publish: the send queue's
   depth less the status frame, so a publish never pushes out its own
   frames. */
#define CAN_IF_PUBLISH_FRAMES_MAX (FRAME_QUEUE_DEPTH - 1u)

/* What the image publishes: its own status frame (VCU_DaqStatus or
   BMS_DaqStatus, the same 8-byte layout, can/daq.dbc) and, if
   build_frames is set, the frames it gives at each publish. */
typedef struct {
    uint32_t status_frame_id;
    uint8_t  status_frame_len; /* the frame's LENGTH in can_daq.h */
    void   (*build_status)(const daq_status_t *in, uint8_t *frame);
    /* Optional, run by daqTask at each publish after the status frame:
       fills out (ID, length, data) with at most max frames for the
       publish'th publish (0 the first), and returns how many. Frames
       longer than 8 bytes are not sent on a classic bus. */
    size_t (*build_frames)(uint32_t publish, can_frame_t *out, size_t max);
} can_if_publisher_t;

typedef struct {
    bool     standby;          /* last commanded STBY state */
    bool     terminated;       /* the relay as commanded now -- NOT a
                                   measured fact (termination.h) */
    bool     fd_capable;       /* the bus's compiled FrameFormat (Classic vs FD_BRS) */
    bool     bus_off;
    bool     error_passive;
    uint32_t last_error_code;  /* FDCAN_LastErrorCode value, see HAL header */
    uint32_t tx_error_count;
    uint32_t rx_error_count;
    uint32_t rx_count;         /* frames successfully queued to canRxTask */
    uint32_t tx_count;         /* frames handed to the TX hardware FIFO */
    uint32_t rx_drop_count;    /* ISR->queue drops (rxQueue full) */
    uint32_t tx_queue_dropped; /* oldest frames pushed out of the send queue */
    uint32_t tx_rejected;      /* refused by can_send(): FD frame on a classic
                                   bus, or too long for its format */
    uint32_t published;        /* publishes, each one status frame queued */
    uint32_t bus_offs;         /* bus-offs seen by daqTask since power-on */
    uint32_t restarts;         /* bus-off restarts, once per second while off */
} can_status_t;

/* Creates the bus's receive queue, mutex and wake semaphore, configures
   the accept-all global filter (mandatory -- CubeMX never generates this
   call, and with zero filter slots configured the peripheral rejects every
   frame by default), starts FDCAN2, activates the RX FIFO0 interrupt, and
   brings STBY low (out of standby) per the ADM3057EBRWZ's confirmed
   polarity, then sets termination to terminate, the image's CAN2
   termination Parameter (termination.h). Unlike CAN1, FDCAN2 is started
   before termination is written and has no settle time: the DAQ bus
   carries nothing safety-related. publisher must outlive daqTask.
   Must run after parameters_init() and before canRxTask/daqTask are
   created -- see tasks_init(). Returns false if a queue, the mutex or the
   controller could not be set up. */
bool can_if_init(const can_if_publisher_t *publisher, bool terminate);

/* Queues one frame for daqTask to send. Never waits: a full send queue
   drops its oldest frame (counted in tx_queue_dropped) to make room.
   False, without queuing, only for a frame the bus cannot carry: is_fd on
   a classic-compiled bus, or len out of range for the format. Any task;
   not from an interrupt. */
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

/* Explicit manual override for testing the STBY pin directly — separate
   from can_if_init()'s automatic out-of-standby-at-boot behavior. The
   termination override is the shell's `term` (termination.h). */
void can_set_standby(bool standby);

/* Task entry points, created once each from tasks_init(). canRxTask drains
   the receive queue; daqTask sends, publishes and handles bus-off, so a
   slow sender never delays draining the receive queue. */
void canRxTask(void *argument);
void daqTask(void *argument);

#endif /* APP_INC_CAN_IF_H */
