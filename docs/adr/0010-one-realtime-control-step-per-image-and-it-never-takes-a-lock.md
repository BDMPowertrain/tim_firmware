---
status: accepted
---

# Each image has one Realtime control step, and it never takes a lock

Each image runs exactly one task at Realtime priority: its control step. On the VCU it runs every 10 ms and does the whole torque path, from the input snapshot to the inverter command. On the BMS it runs every 20 ms and does the sweep, the checks and BMS_OK. The control step is the only code that writes a safety output or sends a Control bus frame, including the slower status and limit frames. It takes no mutex and waits on nothing but its own period and, on the BMS, its own cell-monitor exchange. Data crosses to and from other tasks by a copy made with interrupts briefly masked: a mailbox snapshot on the way in, one published snapshot on the way out, and a zero-timeout queue put for fault-log events.

Accepted for now. The conditions under which it should be revisited are listed under Consequences.

## Considered options

- **A separate send task for the Control bus.** Rejected: building a frame costs microseconds, so nothing is saved. BMS_Status is not soft traffic (the VCU cuts torque without it), so the task would need to sit just under Realtime anyway. Two senders would compete for the controller's three send slots, where a frame with no free slot is skipped. A status frame built outside the step can describe a different step than the one that set the output.
- **A mutex with priority inheritance around shared state.** Rejected: the Realtime task would wait for whatever lower task holds the lock, so the deadline argument would depend on the worst hold time of every holder, re-proven whenever code is added. With no shared lock, nothing lower can hold the control step up and the claim is checkable in review.
- **Running the step from a hardware timer interrupt.** Rejected: up to 1 ms of jitter from the RTOS tick is small against a 20 ms pedal-to-command deadline and a 10 ms BMS output budget, and a task can sleep during the BMS's 7.5 ms link exchange where an interrupt handler cannot.

## Consequences

- A masked section may contain a copy and nothing else. The largest is the BMS published snapshot, about 500 bytes.
- A fault-log event that finds its queue full is dropped and counted; the control step never waits for the writer.
- The step measures its own worst-case time every run. The budgets are 1 ms of CPU on the VCU and 3 ms on the BMS, both estimates until measured.
- **When to revisit.** If the measured worst case passes its budget, or heavy computation arrives (traction control, state-of-charge estimation), the heavy work moves to a lower task that hands its latest result to the step through the same snapshot copy; the step keeps only the final application, which may only reduce torque. If the copies grow until masking interrupts for their length threatens CAN reception, replace the copy with a lock-free hand-over before reaching for a mutex.
- The data log adds a third way out of the step: a ring that only the step writes and only the SD logger reads, with no lock and no masked section (ADR 0021).
- **If safety work is ever split across more than one task,** the watchdog scheme changes with it: a supervisor task takes over the kick (ADR 0013).
- Flash erases must not stall the control step. On this MCU that means data pages in the bank the image does not run from; the flash layout decision owns it.
