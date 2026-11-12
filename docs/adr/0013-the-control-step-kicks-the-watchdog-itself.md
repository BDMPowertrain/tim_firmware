---
status: accepted
---

# The control step kicks the watchdog itself; there is no supervisor task

Each image has one kick call, at the start of its control step, made only if the previous step ran every phase in order to the end. No other code kicks once the control step is running; before that, initialisation kicks at two fixed points. There is no supervisor task and the High priority band stays empty. The independent watchdog is set to 60 ms nominal in both images (56.5 to 65 ms as its oscillator moves between 34 and 29.5 kHz), with the window feature off. On the BMS that puts BMS_OK low at most about 65 ms after a hang, inside the 100 ms requirement of ADR 0008.

## Considered options

- **A supervisor task at High priority that collects check-ins and owns the kick.** Rejected while each image has a single safety task (ADR 0010): it would check one check-in, and it is a second piece of code that can keep kicking after the control step has died.
- **Kick at the end of the step.** Rejected: the gap between kicks would vary with step length (9 to 17 ms of work on the BMS). At the start the gap is one period, and gating on the previous step's completion still proves the work was done.
- **80 ms timeout.** Rejected: it tolerates two lost BMS steps instead of one, but its worst case of 87 ms leaves 13 ms to the requirement.
- **The window feature, to reject kicks that come too early.** Rejected: its exact behaviour is not verified against the reference manual, a false trip costs a power cycle on the BMS, and a single call site already covers a loop that kicks fast.
- **Gating the kick on the soft tasks.** Rejected: a stuck SD card would put the BMS in a held fault or drop the VCU out of Ready to Drive. A stalled background sets a warning and resets nothing.

## Consequences

- **When to revisit.** The moment any safety work moves out of the control step into a second task, a supervisor task takes the High band, every safety task checks in to it each period, and the supervisor becomes the only kicker. A kick owned by one of two safety tasks would hide the death of the other.
- One lost BMS step (40 ms between kicks) is tolerated; two in a row reset the board. The VCU tolerates four.
- An overrun that still finishes keeps the watchdog fed, so it is handled separately: three overruns within one second are a fault (a BMS fault on the BMS, a torque cut on the VCU).
- Nothing may stall the CPU for more than about 35 ms on the BMS. Flash erases are the known case; the flash layout decision owns it.
- No start-up stage may run longer than about 50 ms between kicks, so start-up work in the control step is spread over steps and never blocks.
- No code can run just before a watchdog reset (this watchdog has no early-warning interrupt), so the breadcrumb is the only record of a hang.
