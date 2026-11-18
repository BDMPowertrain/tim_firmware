---
status: accepted
---

# The BMS controls the charge on the highest cell, and the charger's voltage regulation is not relied on

In charge mode the BMS asks the charger for its charge limit, which it tapers to zero as the highest cell nears the charge target and as cell temperature nears either charge limit. The charge ends when the charge limit has stayed under 0.5 A for 10 s with the highest cell at the target, and it does not restart until a power cycle. The voltage in the request is 128 x the charge target and is only a loose backstop.

The charger's own constant-voltage stage was rejected as the control because its accuracy is +/-1%, which is +/-5.4 V on the pack or about 42 mV per cell, and because it sees pack voltage and not the highest cell. Either error alone can carry a cell past the 4.195 V over-voltage fault.

## Considered options

- **Let the charger run CC-CV to a pack voltage.** Rejected for the accuracy and imbalance reasons above.
- **Stop the current, let the cells rest, measure, and repeat.** Rejected: at the end of the taper the current is under 0.5 A, which lifts a cell reading by about 4 mV on the highest-resistance module, the same size as the measurement error. The pause adds states and buys nothing. The rested highest and lowest cell are logged 60 s after the stop instead.
- **Restart when the cells relax below the target.** Rejected for now: a final stop cannot oscillate. The balancing decision may reopen it.

## Consequences

- The charge target is a stored parameter bounded to 3.60-4.18 V with an unset default of 4.10 V. It is a stopping point and not a protection threshold, so ADR 0015 holds. At a 4.18 V target the charger's +1% makes the voltage request useless as a backstop; the over-voltage fault taking BMS_OK low is the protection.
- The charge limit is built like the discharge limit of ADR 0018: the lowest of a full charge limit parameter (bounded to the charger's 12 A, unset default 2 A), the voltage taper (from 50 mV under the target to the target) and the temperature tapers (50 to 55 C, 10 to 5 C), with the same bounded rates. In the car it is 0 A. The taper points are stored parameters with those unset defaults, as the discharge taper points are (ADR 0018), each bounded so no setting puts a taper's zero past its fault threshold: the hot zero under the 58 C over-temperature fault, the cold zero above the 2 C under-temperature fault.
- No charger status flag is a BMS fault. A charger that delivers more than 1 A over the request for 5 s, or more than 1 A for 5 s after a stop, is: BMS_OK is the only hold the BMS has on a charger that ignores CAN.
- Charging starts without operator action at the BMS, so the operator's controls are the charger's supply and the Charging Shutdown Circuit.
