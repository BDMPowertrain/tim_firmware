---
status: accepted
---

# Balancing is decided by firmware, runs only in charge mode, and pauses once a second to measure

The BMS balances only in charge mode. Firmware chooses the cells from a sweep taken with every balancing FET off and writes the FET set to the cell monitors once a second; the monitors' automatic balancing mode and voltage-stop threshold are not used. Each second is about 900 ms of bleeding, then a pause, one settled sweep and the open-wire check. Only that paused sweep feeds the balancing choice, the charge taper and the end-of-charge test.

Balancing is permitted only while the shutdown circuit sense reads closed, BMS_OK is high, no BMS fault is active and the charger has been heard within 5 s (EV.8.3.3, which also covers the Charging Shutdown Circuit). Each bleeding channel is armed with the monitor's 10 s timer and re-armed every second, and the first chain command on every boot is a balancing stop, so a hung or reset MCU cannot leave a cell bleeding with the circuit open.

## Considered options

- **Let the cell monitor decide (auto mode with its voltage-stop threshold).** Rejected: the threshold tops out at 4.0 V in 25 mV steps, it is one absolute level per chip when balancing needs "above the lowest cell in the pack", and it borrows the under-voltage comparator that protection needs.
- **Chip auto mode for the odd/even swap only.** Rejected: firmware still picks the cells, and the chip's swap would run on its own clock instead of lining up with the once-per-second pause. How the 10 s re-arm interacts with the chip's duty timer is unknown.
- **Balance in the car as well.** Rejected: under 0.2 A does nothing useful against drive currents, heats the slave boards inside the accumulator, and disturbs the sag-compensated cell voltages that derating uses (ADR 0018).
- **Trust every sweep while bleeding.** Rejected for now: the slave shares one tap wire and fuse between the sense and balance paths, so a bleeding cell and its neighbour read wrong by an estimated 2 mV per 10 mohm. The pause can go if the bench measures the error under about 2 mV.
- **Automatic top-up charge after balancing.** Rejected: ADR 0019's final stop is kept. A top-up is a power cycle and a second charge.

## Consequences

- A session starts when the cell spread is over 10 mV, bleeds every cell more than 5 mV above the lowest, and is done at 5 mV or less. This is tighter than the 10 mV Orion recommends and sits at the edge of the +/-3.5 mV cell accuracy; loosen it if cells hunt on the bench.
- The balancing gate is 100 mV under the charge target and latches for the charge once the highest cell crosses it. Balancing stops for the session if the lowest cell falls more than 200 mV under the target, or 12 hours after Charge complete; the 12 hours count through pauses.
- Balancing continues after Charge complete, so the hand cart is left powered with the charger connected. A silent charger pauses balancing; it resumes when the charger returns.
- Firmware pauses a board at 95 C die temperature (resume 85 C) and all boards at 45 C cell temperature. The monitor's own 105 C pause is the backstop; its thermistor-based pause cannot work with the Enepaq sensors.
- No balancing failure is a BMS fault. A failed balancing-FET check (run once per charge), a rejected configuration or a spread that never converges stops balancing and is logged; the cell voltage faults remain the protection.
- Every balancing number is a compile-time constant. They are not protection thresholds, but they are tuned by reflash, not stored.
- While bleeding, the 20 ms protective sweeps carry the tap-wire error. The bench must measure it before the margin to the 4.195 V over-voltage fault is trusted.
