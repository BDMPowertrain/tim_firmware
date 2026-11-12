---
status: accepted
---

# The BMS fault latch lives on the shutdown board, not in firmware

The rules need the Tractive System to stay off after a BMS fault until a person at the car resets it (EV.8.2.3). That latch is the shutdown board's hardware latch with its external reset button. The BMS image does not latch: it drops BMS_OK when a fault's debounce expires and raises it again once every value has been back in range for 1 s. Pressing reset while the cause is still present does nothing, because BMS_OK is still low.

One case is held in firmware. After any reset that is not a real power-on (watchdog, software, fault handler) the BMS comes up with BMS_OK low and keeps it low until a power cycle. A cell value can be seen to recover; a firmware crash cannot be shown to have gone away.

## Considered options

- **Latch in firmware until power cycle.** Rejected: it duplicates the hardware latch, and it makes the reset button useless without also cycling GLV power.
- **Store the latch in flash across power-off.** Rejected: the master board has no input to clear it. At power-up BMS_OK stays low until the checks pass, so a fault still present never lets it rise.

## Consequences

- A fault that clears by itself leaves no live trace, so every fault has an *active* bit and a *seen* bit (set until power cycle) in BMS_Status, and every trip is written to the fault log.
- The BMS reads the Shutdown Circuit sense at the end of the loop. BMS_OK low for 500 ms with the loop still energised is a BMS internal fault (welded relay or failed latch). Firmware can only report it.
