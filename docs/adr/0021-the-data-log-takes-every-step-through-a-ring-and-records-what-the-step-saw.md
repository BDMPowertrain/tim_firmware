---
status: accepted
---

# The data log takes every control step through a ring, and records what the step saw, not the bus

Both images keep a data log on the SD card whenever a card is fitted. At the end of each step the control step copies one record into a fixed ring that only it writes and only the SD logger task reads. There is no lock and no masked section: the step writes the record and then advances its index. If the ring is full the new record is dropped and counted, and the step never waits. The ring is 64 records deep on the VCU (640 ms) and 16 full snapshots deep on the BMS (1.6 s at one snapshot every fifth sweep).

A step record holds the inputs the step used (each mailbox's contents with its NODATA/VALID/INVALID state, the pedal and brake values) and what it decided (state, torque request, limiter and cut bits, the command it sent). The data log holds no raw trace of the Control bus. The VCU does record the DAQ bus as raw frames, through a queue that may drop.

This adds a third way for data to leave the control step, next to the published snapshot and the event queue of ADR 0010. It is the lock-free hand-over that ADR names as the step before a mutex.

## Considered options

- **The logger copies the published snapshot.** Rejected: the snapshot holds only the latest step and an SD card routinely stalls for 100 to 250 ms, so steps would be lost every time the card paused. Proving the 80 kW rule windows from a log needs every step.
- **An RTOS queue from the step to the logger.** Rejected: a queue put masks interrupts for the length of the copy and runs kernel code in the step. A ring with one writer and one reader needs neither.
- **A raw trace of every Control bus frame.** Rejected: the CAN1 receive interrupt writes straight into mailboxes with no queue (ADR 0004), so a trace would add work to that interrupt. A trace also shows what was on the wire, not what the step used, and the question a log is opened for is why the step cut torque. A raw trace is the bench CAN interface's job.
- **A separate datalogger node on the DAQ bus.** Not needed: at about 20 KB/s the logger uses a few percent of the CPU once SPI2 runs at about 10 MHz. Left as a future effort if the bench measurements fail.

## Consequences

- No card state is ever a fault and none blocks the start procedure. A missing, removed, slow or full card costs records, never a deadline.
- The logger is a BelowNormal task. With logging running, idle stays at or above 50% and the worst-case step time is no worse than with logging off; both are bench measurements, repeated with the card pulled mid-write and with a slow card.
- If a measurement fails, the retreat order is: drop the DAQ bus recording on the VCU, then lower the IMU rate. The step records go last.
- Every event is also written to the data log as a record carrying the same 32 bytes. The event log stays the authority (ADR 0014); the copy lets the two be lined up by sequence number, boot number and uptime.
- A gap in the record sequence numbers is a visible loss, and the dropped count is reported in the status bits on the DAQ bus.
