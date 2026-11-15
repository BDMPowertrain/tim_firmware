---
status: accepted
---

# The BMS owns derating, and tapers on sag-compensated cell voltage

All derating for cell voltage and cell temperature is done in the BMS by lowering the discharge limit it sends; the VCU obeys that limit and never sees a cell value. The voltage taper works on the minimum cell voltage with sag added back (measured voltage + pack current x a cell resistance parameter), not on the loaded voltage. The VCU adds no derate of its own for motor or inverter temperature either: the inverter already does both.

The loaded voltage was rejected because it closes a loop through the car. Less torque means less sag, so the cell reads higher, the limit rises, torque rises and the sag returns. With a 3.0 V to 2.7 V taper this goes unstable once full-current sag passes about 0.375 V per cell, and a fast-fall, slow-rise limit turns it into a sawtooth lasting seconds: the car surges on the straights late in endurance. Compensating for sag keeps the taper input still when torque changes, so it falls only as the cell empties.

## Considered options

- **The VCU tapers torque itself on the low-voltage bit and cell temperature.** Rejected: two images would each hold cell knowledge, and the limit sent to the inverter on 0x202 would disagree with what the VCU enforces.
- **Taper on loaded minimum cell voltage, with a slow rise.** Rejected: the sag-recovery loop above.
- **A resting voltage sampled only near zero current.** Rejected for now: it needs no resistance value, but it goes stale through a long full-throttle stint, which is when the taper matters.
- **A second derate in the VCU on motor and inverter temperature.** Rejected: it would act on the same temperatures as the inverter's own derating and the two would fight.

## Consequences

- Two linear tapers, the lower wins: 100% at 3.0 V down to 20% at 2.7 V (compensated minimum cell voltage), and 100% at 50 C down to 20% at 57 C (maximum cell temperature).
- The discharge limit falls at up to 200% per second and rises at up to 10% per second, so one bad sweep costs 4% and not the whole limit. The rates are constants.
- The taper break points, the cell resistance and the full discharge limit are stored BMS parameters. This does not contradict ADR 0015: they are derating points, not protection thresholds, and the image bounds each one so no setting can put a taper's end past its fault threshold or the full limit above the cell datasheet maximum. The unset default for the full limit is low.
- The under-voltage fault stays on raw loaded voltage at 2.505 V, so a wrong resistance parameter cannot hide a low cell. With pack current invalid the taper uses raw voltage, the conservative side.
- The cell resistance must be measured on the real pack early; too low leaks sag back into the loop, too high makes the taper act late.
- The discharge limit travels with pack current and pack voltage in one 20 ms frame, BMS_Power, so each frame moves it by at most 4% and the VCU needs no smoothing.
- The inverter's motor temperature parameters (full-torque and zero-torque temperature, over-temperature limit) must be set at commissioning, and the motor's temperature sensor must be wired to the inverter.
