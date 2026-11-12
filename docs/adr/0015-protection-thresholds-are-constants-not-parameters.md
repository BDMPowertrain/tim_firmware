---
status: accepted
---

# Protection thresholds are constants in the image, never stored parameters

A stored parameter is only something a person legitimately sets at the car: a calibration (APPS 1 and 2 minimum and maximum, the brake sensor zero and brake-engaged threshold) or a configuration (CAN termination per bus, maximum torque under a compile-time ceiling). Every protection threshold and timing is a compile-time constant: all BMS voltage and temperature thresholds and debounce times, the pedal plausibility times and percentages, the 80 kW limit, the CAN timeouts and the watchdog timing.

So the BMS has no stored value whose corruption or mis-setting could weaken its protection, and changing a threshold takes a reviewed commit and a new image.

Defaults never allow driving. When no valid parameter copy exists, or a value is missing or out of range, the pedals are "not calibrated", maximum torque is 0 N·m and CAN termination is open. The VCU still reaches GLV_ON, because the shell is needed to calibrate, but treats the pedals as implausible, refuses the start procedure, sets a "parameters defaulted" bit in VCU_Status, shows the fault LED and writes an event. The BMS runs normally and only reports it.

## Considered options

- **Thresholds as parameters with safe defaults and range limits.** Rejected: it lets a threshold be changed in the paddock with no review, and makes the protection depend on flash contents and a CRC.
- **A VCU that stays in STARTUP with bad parameters.** Rejected: the shell must work to calibrate, and GLV_ON with zero torque and no start procedure is as safe.
- **Driveable defaults for calibration (nominal sensor end points).** Rejected: a nominal end point is a guess about a mechanical pedal box.

## Consequences

- Parameters are stored as two full copies of (ID, value) pairs driven by one parameter table (ID, name, type, default, minimum, maximum). Each copy has a marker, the image that wrote it, a save counter, a length and a CRC-32. A save overwrites the older copy: erase, write, read back and compare.
- Loading takes the newest copy with a good CRC and the right image. An unknown ID is ignored, a missing ID gets its default, an out-of-range value gets its default and an event. A new image that adds a parameter keeps the existing ones.
- Parameters are written only from the shell and take effect only after the next reset; the control step reads them once at start-up. The VCU accepts a save only in GLV_ON, the BMS at any time.
- Tuning a threshold at an event means building and flashing an image.
