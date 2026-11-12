---
status: accepted
---

# A brown-out counts as power-on; only the BMS holds after any other reset

The MCU's reset-cause register has no power-on flag: switching GLV on and a supply sag below the brown-out threshold both set the same brown-out (BOR) flag. Both images therefore treat "BOR flag set" as a power-on reset. After any reset without it (watchdog, software, reset pin alone, option-byte load) the BMS holds BMS_OK low until the next power-on reset (state FAULT_HELD, ADR 0007). The VCU never holds: after any reset it runs STARTUP, lands in GLV_ON and waits for the start procedure, with the cause logged and a "last reset was not power-on" bit in VCU_Status. On the BMS a reset with a debugger attached (core debug enable bit set) is treated as power-on, in every build.

## Considered options

- **Tell a brown-out from a power-on with a marker kept across reset.** Rejected: a sag deep enough to trip the brown-out reset wipes RAM and stops the MCU exactly as switching off does, and the BMS repeats every startup check before BMS_OK rises. Provisioning raises the threshold to about 2.8 V so the reset happens while the SD card (2.7 V minimum) and the analog readings are still in spec.
- **Hold the VCU down too.** Rejected: the start procedure is already the deliberate human act that guards torque, so a VCU that reset-loops cannot reach Ready to Drive unattended. Holding would only strand a car on track.
- **Strict hold on the BMS even under the debugger.** Rejected: every flash would need a bench power cycle. The debug bit cannot be set without a probe on the SWD header and stays set until a power cycle, so a car with no probe always gets the strict rule.

## Consequences

- The reset-cause register is read once, before anything else in `main()`, kept, then cleared.
- A supply that sags and recovers on the BMS is not a held fault. It shows as a power-on reset in the fault log.
- A BMS that was debugged and then left connected runs without the hold until it is power-cycled.
