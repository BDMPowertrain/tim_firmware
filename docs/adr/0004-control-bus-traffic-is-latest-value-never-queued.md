---
status: accepted
---

# Control bus traffic is latest-value, never queued

On the Control bus a late frame is worse than a missing one: it carries a valid CRC and counter, so the receiver acts on old data, while a missing frame is caught by the receiver's timeout. Both directions are therefore built so that nothing can wait in software. Each received frame overwrites its mailbox from the receive interrupt, and each periodic frame is built from current values when it is due and handed straight to one of the controller's three send slots, which transmit in ID-priority order. If no slot is free the frame is skipped and counted, and the next period builds a fresh one.

## Considered options

- **Receive and send queues with worker tasks (what the practice firmware does).** Rejected: a send queue releases a backlog of stale frames after a bus disturbance, and a receive queue can overflow or hand a reader frames out of date by an unknown amount. The G474 holds only three received frames per FIFO, so the interrupt has to be short either way.
- **A priority-ordered software send queue in front of the hardware slots.** Rejected: neither image sends enough Control bus frames to need it. The VCU sends two; the BMS sends seven, offset in time so no more than three are ever due together.
- **Decoding and checking frames in the receive interrupt.** Rejected: it lengthens the interrupt and puts the CRC, counter and ageing checks where host tests cannot reach them.

## Consequences

- A mailbox holds the latest raw frame, its arrival time, a validity state (NODATA, VALID, INVALID) and error counters. It cannot overflow.
- Each control task takes one snapshot of the mailboxes it uses at the start of its step, and decodes and checks them there. That code is logic and is host-tested. A value cannot change partway through a step.
- A reader whose mailbox is not VALID is given the frame's substitute, never the last real value.
- The BMS's periodic frames need a fixed offset table. Adding a Control bus frame means checking that no more than three are due on any tick.
- The DAQ bus is the exception: it has a send queue that drops its oldest frame when full, because its data is for logging and a caller must never wait on it.
