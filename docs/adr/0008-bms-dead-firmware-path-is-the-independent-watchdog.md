---
status: accepted
---

# The BMS has no external watchdog; the MCU's independent watchdog is the dead-firmware path

BMS_OK is a 2-input AND of MCU_BMS_OK and the cell-monitor bridge's NFAULT. When the firmware hangs, the MCU's independent watchdog resets it, every pin floats, the pull-down on MCU_BMS_OK takes it low, and BMS_OK drops. The requirement is BMS_OK low within 100 ms of the firmware hanging. The firmware then comes up holding BMS_OK low until a power cycle (ADR 0007).

## Considered options

- **External window watchdog (TPS3430) as a third AND input.** Rejected by the hardware owner to keep the master board simple. It would have covered a firmware that hangs while still kicking the watchdog, and an MCU whose clock or watchdog has failed.

## Consequences

- The accepted gap is a hang that keeps the watchdog fed. The watchdog supervisor must make that hard: the kick depends on every safety task checking in, never on a timer or idle loop alone.
- NFAULT has a pull-up, so a dead bridge chip reads "no fault". Firmware covers it by comparing the NFAULT readback with the bridge's fault register over SPI every sweep.
- The stack's hardware comparators only reach NFAULT while firmware is polling, so they back up wrong firmware judgement, not dead firmware.
