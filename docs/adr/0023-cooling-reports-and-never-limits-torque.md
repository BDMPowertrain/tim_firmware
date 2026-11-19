---
status: accepted
---

# Cooling reports and never limits torque; the inverter protects itself

The VCU runs the radiator fan and the coolant pump, but cooling never lowers torque. No cooling fault, sensor failure or high temperature puts a second ceiling beside the **Power limiter**, and none is a **Torque cut** cause. Overheating is the CM200DZ's job: it already cuts its own current limit on its hot-spot, coolant and motor temperatures (SUM p.86, the Internal States limit bits). A failed cooling input (the coolant NTC out of range, or the inverter's temperature frames 0x0A0 and 0x0A2 not VALID) is read as "too hot", so the fan and pump run at full and the failure is reported, never hidden. A fan or pump current-sense fault (over-current, open load, stuck on) and a failed expander only report: there is nothing the VCU could do about them that would help.

The decision is made by logic in the **Control step**, which already has the state, the mailboxes, the ADC window and the reporting paths (VCU_Status, the step **Record**), and returns a fan and a pump duty. **Housekeeping**, which already owns the I2C expander for the **Lamp check** (ADR 0022), rewrites every expander output from those duties every 100 ms and hands back whether the write succeeded. The step never touches I2C (ADR 0010).

## Considered options

- **A VCU thermal derate through a second limiter.** Rejected: the inverter derates on the same temperatures, measured closer to the heat, every 3 ms. A second derate in the VCU would act later on worse data and add a torque-cut cause that a failed sensor could trigger by itself, against what issue 20 decided for the coolant sensor.
- **A failed input substituted as "cold", or dropped from the hottest-of.** Rejected: a dead sensor would then quietly stop the fan in a hot car. Full cooling costs only GLV current and noise.
- **The whole cooling function in Housekeeping.** Rejected: its faults and readings would need a second path back to the step to reach VCU_Status and the **Data log**. The Housekeeping hand-off only delays the outputs by up to 100 ms, which cooling does not notice.
- **The expander written only on a change.** Rejected: the expander does not reset with the MCU, and one that browns out comes back with its outputs off. Rewriting every output every 100 ms recovers it without the step knowing.

## Consequences

- The cooling thresholds and current-sense limits are constants (ADR 0015), placeholders until the fan, pump and PDU hardware are chosen.
- A stalled Housekeeping holds the expander's last outputs; Background stalled reports it after 1 s.
- An inverter that loses its temperature broadcasts runs the fan and pump at full until they return; the mailbox-timeout Event records it, and InverterTimeout (a torque-cut cause) does not.
