# Power limiter and battery-limit derating: how single-motor FSAE EV teams control it without torque hunting

Research note, 2026-11-12. Planning input only; no firmware follows from this file directly.

Question: how do Formula SAE Electric teams with one motor and one Cascadia Motion (RMS) inverter in CAN torque mode implement the 80 kW limiter and battery-limit torque derating in the VCU, and how do they stop the control from making motor torque oscillate?

How to read this note:

- Every claim carries a source tag such as [CM p.97] or [LH-1]. Tags are resolved in "Sources" at the end.
- Code citations are pinned to a commit. Line numbers refer to that commit.
- Items marked **Unverified** could not be checked against a primary source.
- Items marked **Analysis** are our own reasoning, not something a source states.
- Cascadia manual page numbers are the printed footer numbers of document 0A-0163-04. The PDF page index is the printed number plus one.

## 1. Summary

1. Every public Cascadia VCU we could read limits power with a **feed-forward torque ceiling from P/omega**. Berkeley and Waterloo apply it to the torque command with no feedback on measured power [FEB-1][WAT-1]. Kennesaw State hands the same calculation to the inverter [KSU-1]. Texas adds a slow integral trim on top [LH-1].
2. The one team that documented a closed-loop PI on measured power **removed it because it overshot and cycled between full torque and zero**. It used a 10 Hz V x I measurement, large gains and a large integral store [LH-2][LH26-1]. This is the clearest public account of the hunting problem and of its fix.
3. The fix they chose is the same shape as our plan: P/omega ceiling, plus an integral-only trim that updates once per new current sample, integrates only while the ceiling is binding, is clamped, and only ever reduces [LH-1].
4. No public code we found rate-limits or filters the power-limit ceiling itself. Stability comes from the structure (no measured-power feedback in the fast path), not from smoothing [LH-1][FEB-1][WAT-1].
5. Battery derating is done two ways. Berkeley tapers on **loaded** pack voltage and warns in its own code to "bench-tune to avoid oscillation" [FEB-2][FEB-4]. Texas derates on an **open-circuit estimate** of minimum cell voltage that only updates at rest, which removes the sag feedback path entirely [LH-3]. The Orion BMS computes its discharge limit from open-circuit voltage and cell resistance, to keep cell voltage in range "preemptively" [ORION p.13].
6. Reported setpoints sit well below 80 kW: 70 kW electrical [LH-1], 62 kW mechanical [WAT-1], 63 kW mechanical [HT-1]. Our 76 kW target is at the aggressive end of what is published.
7. The inverter can do part of this job itself. Its 0x202 BMS-limit feature is exactly a P/omega ceiling computed inside the inverter, but it ignores efficiency, so it under-limits [CM p.97]. One team uses it as their whole power limiter [KSU-1].

## 2. The rule the limiter is judged against

- Maximum power measured by the Energy Meter must not exceed 80 kW [RULES EV.3.3.1].
- A violation is exceeding it continuously for 100 ms or more, or after a 500 ms moving average is applied [RULES EV.3.4.1].
- Any algorithm that adjusts requested wheel torque may only decrease the driver's request [RULES EV.3.1.3].

**Analysis.** The two tests punish different errors. A short spike under 100 ms that the 500 ms average absorbs is legal. A steady error of a few percent is not. So the dangerous error is a **steady bias** in the ceiling (wrong efficiency, wrong sensor gain), not a brief transient. A slow trim cannot protect against bias in its first second; only margin can.

## 3. Control structures actually used

### 3.1 Feed-forward ceiling only

**Berkeley (EMRAX, RMS PM100DX).** Maximum torque is `min(MAX_TORQUE, peak_current x pack_voltage / motor_speed)`. Pedal position scales that maximum. There is no efficiency term, no feedback on power, no rate limit and no filter. Below 15 rad/s the P/omega term is skipped and the static torque limit applies [FEB-1]. Pack voltage is the measured terminal voltage from an IVT sensor, with inverter DC bus voltage as fallback [FEB-2]. Their previous car used a fixed 400 V in the same formula, which is the most conservative choice [FEB-3].

**Waterloo (Cascadia CM200DZ).** `maxTorqueDemand = min(maxTorqueDemand, INV_POWER_LIMIT / (motor speed))` with `INV_POWER_LIMIT 62000.0` and a code comment that this is the 80 kW limit. Inverter V x I is computed and broadcast for logging but is not in the control path [WAT-1]. The 0x0C0 torque limit field is sent as 0, so the inverter uses its EEPROM limits [WAT-2]. **Unverified:** whether this car has one motor or two; a comment elsewhere in the same repository says current limits "are per motor" [WAT-3].

**Texas, 2026 car (Cascadia inverter).** A torque-versus-rpm lookup table shapes available torque; the pedal picks a fraction of it [LH26-2]. That same build also ran the PI described in 3.2.

**Georgia Tech (four AMK motors, included for the structure only).** A mechanical power limit of 63 kW is applied by scaling torque so that the sum of torque x speed stays under it. A PID power limit option exists in the configuration and is switched off [HT-1].

### 3.2 PI on measured power, and why it was dropped

Texas ran a one-sided PI on battery V x I: `trim = kp x error + ki x integral`, clamped so it can only reduce torque, with `kp = 0.006`, `ki = 0.6`, integral clamp 20 000 W s, limit 70 kW [LH26-1]. A low-pass filter on the power measurement is present but commented out [LH26-1].

The commit that replaced it states the failure directly: "The old PI acted on 10 Hz V x I with large gains and a 20,000 W s integral, so it overshot and could cycle between full torque and zero" [LH-2].

**Analysis.** Those numbers explain the cycle. With `ki = 0.6` Nm per W s and a 20 000 W s clamp, the integral alone can demand 12 000 Nm of trim, so once it has wound it pins torque at zero until it unwinds. With `kp = 0.006`, a 10 kW overshoot removes 60 Nm in one step, and the measurement that would confirm the correction arrives up to 100 ms later. High gain plus transport delay plus a deep integral store produces a limit cycle.

### 3.3 Feed-forward ceiling plus a slow, bounded integral trim

Texas, current code [LH-1][LH-4]:

- Ceiling: `cap = (limit_w - trim_w) / (dc_power_ratio x omega)`.
- "Motor speed is fresh every tick, so the cap has no feedback delay."
- Trim is integral-only, in watts, gain 2 W per W s of error, clamped to 0 to 10 kW. It can only lower the ceiling.
- It updates **once per new current sample**, not once per control tick, using the real interval between samples. That interval is clamped to 0.2 s so a gap in data cannot become one large step.
- It integrates **only while the ceiling was binding**, "so the trim never winds while the driver asks for less".
- It is **kept across pulls**, not reset when the driver lifts.
- The ceiling is skipped below 10 rad/s.
- `dc_power_ratio` is not a physical efficiency. It is inverter V x I divided by torque feedback x omega from logs. The logged median was 0.85; the shipped value is 0.95, "at the high end of the spread" [LH-2][LH-5]. So their spread in that ratio is on the order of 10%.

Their evidence that it does not hunt is a simulation, not a car. A closed-loop host test with a 5% ratio error peaks at 42.3 kW against a 40 kW limit, settles at 40.0 kW, and the largest one-step torque drop is 1.3 Nm [LH-5][LH-6]. The same pull request says "Not run on hardware" and that with their current torque map the ceiling never binds [LH-5].

**MIT (EMRAX, Rinehart).** A 2019 thesis describes a feed-forward mechanical power limiter combined with an electrical limiter that closes a PI loop on battery power, in C on the VCU [MIT-1]. **Unverified:** gains, loop rate, anti-windup and results. The thesis body could not be retrieved (the repository rate-limited us); only the abstract was read.

A team member's notes from the same period record what made it hard [MIT-2]:

- The inverter tracks an Iq target derived from the torque command, so produced torque often differs from commanded torque.
- The inverter's torque feedback assumes a linear Iq-to-torque relationship, which is not true.
- Efficiency computed from commanded torque came out above 1 or below 0.
- Field weakening changes efficiency and its onset depends on bus voltage, which sags as the pack drains.
- At the 2019 competition they simply cut the driver's maximum torque by about a third.

**Unverified:** a second post from that author reportedly records 88 kW measured against a predicted 65 kW from a torque-limit-only approach, and negative torque overshoot when the command was dropped quickly. The post returned 404 and we only saw it through a search summary.

### 3.4 Reactive scaling on measured power

UC Irvine's documented limiter multiplies the torque command by `threshold / measured_power` whenever V x I exceeds the threshold, on the stated assumption that torque and power are proportional [AER-1]. Kennesaw State has a commented-out version that applies an exponential decay to torque once V x I exceeds the limit [KSU-2].

**Analysis.** Both act only after the limit is already exceeded and both are proportional feedback through a delayed measurement. They are the structures most likely to chatter at the limit. Neither source reports test results.

### 3.5 Letting the inverter do it through 0x202

Kennesaw State's PM100 car sends the inverter a BMS current-limit frame on 0x202 from the VCU on a 10 ms timer, with discharge limit `power_limit / bus_voltage` capped at 280 A [KSU-1]. The frame is only sent when the limit is set below 80 000 W; at the default 80 000 it is not sent at all [KSU-1]. See section 6 for what the inverter does with it.

### 3.6 Which measurement and how fast

| Team | Power measurement | Rate | In control path? |
|---|---|---|---|
| Texas, current | Inverter DC bus V (0x0A7) x I (0x0A6) | per new CAN sample | Trim only [LH-1] |
| Texas, 2026 | Battery V x I | 10 Hz | Yes, PI. Hunted [LH-2] |
| Berkeley | IVT pack voltage (feed-forward term); IVT current (backstop) | not stated | Voltage yes; current only above limit [FEB-1] |
| Waterloo | Inverter V x I | per command | No, logging only [WAT-1] |
| Kennesaw State | Inverter DC bus voltage | 100 Hz | Sets the 0x202 limit [KSU-1] |

Two facts from the Cascadia manual bear on the choice:

- Inverter DC bus voltage is "the actual measured value". DC bus current is "the calculated DC Bus current" [CM p.144]. Inverter V x I is therefore an estimate, which fits the 10% spread Texas saw.
- Broadcast frames are sent from a 3 ms loop, so a 10 ms setting gives about 12 ms in practice and the interval varies [CM p.82].

No public source we found uses the competition Energy Meter in the control path.

## 4. Anti-oscillation techniques found, and the evidence for each

| Technique | Who | Evidence |
|---|---|---|
| Keep measured power out of the fast path; compute the ceiling from speed | Texas, Berkeley, Waterloo, Georgia Tech | [LH-1][FEB-1][WAT-1][HT-1] |
| Integral-only trim, no proportional term | Texas | [LH-1] |
| Integrate once per new sample, with the real sample interval, interval clamped | Texas | [LH-1] |
| Conditional integration: only while the ceiling binds | Texas | [LH-1] |
| Small clamp on trim authority (10 kW of 70 kW) | Texas | [LH-1] |
| One-sided trim: can only lower the ceiling | Texas | [LH-1] |
| Keep the trim across pulls | Texas | [LH-4] |
| Set the feed-forward ratio at the pessimistic end of the logged spread | Texas | [LH-5] |
| Skip P/omega below a speed floor | Texas 10 rad/s; Berkeley 15 rad/s; Cascadia 0x202 at 525 rpm for a 10-pole motor | [LH-1][FEB-1][CM p.97] |
| Derate on open-circuit voltage estimated at rest | Texas | [LH-3] |
| Compute the current limit from open-circuit voltage and resistance | Orion BMS | [ORION p.13] |
| Asymmetric rate limit: cut at once, restore slowly | Texas, for wheel-slip control only | [LH-4] |
| Anti-windup by freezing the integrator near saturation | Waterloo, for an endurance energy controller | [WAT-3] |

Not found in any public code we read:

- A rate limit or low-pass filter on the power-limit torque ceiling.
- Hysteresis on the power limit.
- Gain scheduling of a power controller with speed.
- A filter on measured power in active use. The only one found is commented out [LH26-1].

On low speed: with the ceiling combined by `min()` with the motor torque limit, P/omega only binds above base speed, so the blow-up near zero never reaches the output. The speed floor is there to avoid a divide by zero, not to shape behaviour [FEB-1][LH-1].

**Unverified:** asymmetric torque rate limiting (fast down, slow up) applied to the power limiter is often described informally, but we found no team source that implements it for this purpose.

## 5. Turning BMS limits into a torque limit without a sag-recovery cycle

The mechanism to avoid: torque is reduced, pack current falls, cell voltage recovers by I x R, the limit rises, torque rises, voltage sags, the limit falls. Sources handle it three ways.

**Use loaded voltage and shape the taper around the expected sag (Berkeley).**

- Current limit is 100% above 460 V, falls linearly to 16.7% (10 A of 60 A) at 410 V, and stays there below [FEB-2].
- The 460 V start is the 400 V floor plus the expected sag at peak current, from a measured figure: "At 510V, commanding 65A causes ~62V drop", so about 1 ohm [FEB-2].
- A second, coarser step drops maximum torque from 220 Nm to 150 Nm below 420 V [FEB-1][FEB-4].
- A backstop scales torque by `limit / measured_current` when measured current exceeds the limit. The comments say "Bench-tune to avoid oscillation before trusting" and "Conservative backstop — bench-tune" [FEB-1][FEB-4].
- No filter, rate limit or hysteresis on any of it.

**Use an open-circuit estimate (Texas).**

- Minimum cell voltage is only sampled when pack current has been under 5 A for 1 s, then smoothed with an exponential average, alpha 0.005 [LH-3].
- Torque is scaled linearly from 1 at 3.2 V to 0 at 2.8 V on that estimate [LH-1].
- Because the estimate cannot move under load, sag cannot feed back. The cost is that it also cannot see a cell collapsing under load.
- In their current defaults this derate is computed and logged but not applied [LH-7].

**Compute the limit from open-circuit voltage and resistance (Orion BMS).**

- The BMS solves "for the maximum possible amperage that will still keep the cell voltage inside the safe range", using "the internal resistance of the cell and the open circuit voltage of the cell". This "preemptively keeps the cell voltage within specifications" [ORION p.13].
- A measured-voltage path exists only as a backup: if a cell goes outside its limit the current limit is cut by 20% of maximum, up to five times, in quick succession [ORION p.13].
- Temperature derating is a table of current limit against temperature in 5 C steps [ORION p.12-13].
- On a critical fault the limit is ramped to zero at a programmable rate rather than stepped [ORION p.14].

**What the inverter does with a BMS limit.** It multiplies the limit by DC bus voltage, which is loaded voltage. No filtering is described [CM p.97].

**Not found:** any team source that states rate limits or filter constants for a BMS-derived torque limit, or that reports a measured sag-recovery oscillation on the car. Berkeley's warning comment is the only direct acknowledgement.

## 6. Cascadia Motion software manual: what matters here

**Command message 0x0C0** [CM p.90]

- Bytes 0-1 torque command; bytes 6-7 "Commanded Torque Limit".
- Limit field: "If set to 0, the default torque limits sets in the EEPROM parameters are used. If set to a positive number then the Motor and Regen torque limits are set to the torque value sent."
- So **0 does not mean zero torque**. A limiter that computes a limit of 0 and writes it to this field gets the full EEPROM limit.
- The field sets motoring and regen limits together.
- The command is processed every 3 ms. Sending faster than 3 ms gains nothing. 10 to 50 ms is recommended for vehicles.

**Torque rate limit** [CM p.121]

- `Torque_Rate_Limit_EEPROM_(Nm)_x_10` "adjusts how quickly the torque command is allowed to change", set as a torque increment every 3 ms.
- For the integrated iM225/iM375 modules the manual recommends no more than 70 Nm per 3 ms and says faster rates "may cause stability issues" [CM p.40].
- The manual describes one rate, not separate up and down rates. **Unverified:** whether it applies equally to decreasing torque.
- In speed mode the regulator output is not rate limited [CM p.61]. Not relevant to torque mode, but it confirms the limiter sits on the torque command path.

**Torque capability** [CM p.35]

- Recomputed every 3 ms from the EEPROM torque limit and the active current limit, which "is influenced by active PWM frequency, bus voltage, stall model status, hot spot model temperature, coolant temperature and motor temperature".
- Readable on CAN broadcast 0xB1.
- The description of this capability limiting the VCU command is given for the catalog integrated modules. **Unverified:** that it behaves the same with a PM100/CM200 and an EMRAX.

**0x202 BMS current limit** [CM p.96-97]

- Bytes 0-1 maximum discharge current in amps, bytes 2-3 maximum charge current, little endian, sign ignored.
- Enabled by `CAN_BMS_Limit_Enable_EEPROM = 1`.
- Times out 1 s after the last frame.
- "The BMS torque limiting function does not use the inverter measured or estimated DC current."
- Formula: `Maximum torque = DC bus voltage * DC Current Limit / speed`, speed in mechanical rad/s.
- It "does not consider motor and inverter efficiency and thus will have accuracy issues that vary with operating conditions".
- Speed has a lower bound, 525 rpm for a 10-pole motor.
- It applies in CAN mode: "it will also limit the torque command coming from the CAN command message".
- Flag `INV_BMS_Limiting_Motor_Torque` in 0x0AA reports when it is active [CM p.148].

**Analysis.** The 0x202 formula caps mechanical power at V x I_limit, so electrical power settles at V x I_limit / efficiency. Battery current will exceed the limit sent by roughly the loss fraction. It is a feed-forward ceiling on the inverter's own speed, so it should not hunt, but it must be sent a deliberately reduced current to be a real limit.

**Motor temperature derating: yes, separately from inverter temperature.**

- `Full_Torque_Temp` and `Zero_Torque_Temp` EEPROM parameters: torque capability falls linearly between them as motor temperature rises [CM p.121, p.123]. Flag `INV_Limit_Motor_Temp_Derate` [CM p.147].
- The motor over-temperature limit parameter trips a fault and turns the inverter off [CM p.117].
- A motor hot-spot model with its own PI regulator derates motor current. It is "only active during stall" and is described for catalog motors [CM p.52]. Flag `INV_Limit_Hot_Spot_Motor` [CM p.147].
- Inverter-side derates with their own flags: inverter hot spot, coolant temperature, low-speed current limiting, stall burst model, maximum speed [CM p.148].

**Analysis.** Every one of these makes delivered torque lower than commanded torque without telling the VCU's limiter. That matters for any integrator in the VCU; see 8.2 A.

**Shudder compensation** [CM p.66]. The inverter can add a correcting torque against driveline speed oscillation, typically at very low speed and moderate torque. It is a separate mechanism from anything above, and worth knowing about if oscillation is seen at low speed rather than at the power limit.

## 7. Margins and overshoot reported

| Source | Setpoint | Basis | Result reported |
|---|---|---|---|
| Texas, current | 70 kW electrical | Inverter V x I, "not checked against the official energy meter" | p99 of logged power 72.9 kW at a 70 kW target by their ratio; the limiter itself tested in simulation only [LH-2][LH-5] |
| Waterloo | 62 kW mechanical | P/omega | None [WAT-1] |
| Georgia Tech | 63 kW mechanical default; 51.5 kW in one configuration | P/omega over four motors | None [HT-1] |
| Berkeley | 60 A x pack voltage, about 30 kW | Accumulator current limit, not the rule | None [FEB-4] |
| Kennesaw State | 80 kW, via 0x202 | No margin; frame not sent at the default | None [KSU-1] |
| MIT 2018 | none effective | | Over the limit for about 5 ms off the line; removed from endurance under that year's rules [MIT-2] |
| MIT 2019 | driver torque cut by about a third | | Unlimited car "will happily use more than 100 kW" [MIT-2] |

**Not found:** any team publishing its limiter's measured behaviour against the 100 ms and 500 ms tests on Energy Meter data.

**Unverified:** a Cal Poly senior project reportedly uses a quadratic torque penalty above a driver-adjustable threshold defaulting to 60 kW. The document returned 403 and was not read.

## 8. What this suggests for our design

Our plan, for reference:

- One 10 ms control step.
- Ceiling = power ceiling x efficiency / speed.
- Bounded integral trim of +/-15% on measured power, from BMS pack current (20 ms frame) x voltage.
- Power ceiling = min(76 kW, BMS discharge limit x voltage).
- 68 kW feed-forward only when the current measurement is lost.
- BMS taper on minimum cell voltage and maximum temperature that falls at once and rises at 10%/s, sent every 100 ms.

Everything in 8.1 to 8.3 is **Analysis** built on the sources above.

### 8.1 What the evidence supports as-is

- The structure is right. It is the one structure with a documented history of replacing a hunting PI [LH-2], and the feed-forward part matches every other Cascadia team read.
- A fallback with more margin when the measurement is lost is consistent with teams that run feed-forward only at 62 to 70 kW [WAT-1][LH-1].
- A 20 ms current frame is five times faster than the 10 Hz measurement that failed for Texas [LH-2].

### 8.2 Flaws and risks in the plan

**A. A trim that can raise the ceiling by 15% can wind up and then overshoot.**

76 kW x 1.15 = 87.4 kW. The integrator sees "measured power below target" whenever delivered torque is below the commanded ceiling for a reason outside the limiter: inverter motor-temperature or coolant derate, hot-spot limiting, voltage-limited operation at high speed, or the 0x202 limit [CM p.35, p.147-148]. It then winds toward +15%. When that condition clears, the ceiling is up to 15% too high and stays there until the integrator unwinds, which by design takes seconds. That is far longer than 100 ms. Texas avoids this by making the trim one-sided, 0 to 10 kW downward only [LH-1].

Recommendation:

- Make the bounds asymmetric: about -15% down, and up only as far as keeps `target x (1 + up)` under the true limit with sensor margin. With a 76 kW target that is roughly +3%.
- Integrate upward only when the ceiling is binding **and** the inverter is delivering it: torque feedback close to command, and none of the 0x0AA limit flags set.
- Prefer starting with a pessimistic efficiency and a one-sided downward trim, as Texas does, and accept the lost kilowatts until there is real data.

**B. The power ceiling `discharge limit x voltage` closes a loop through pack sag.**

The BMS tapers on loaded minimum cell voltage. The loop is: torque -> current -> cell sag -> limit -> torque. Its gain is `taper slope x full current x cell-group resistance`. The planned taper is 80% over 0.3 V, so 2.67 per volt. With that slope the loop gain exceeds 1 once full-current sag exceeds 0.375 V per cell, and exceeds 0.5 at half that. The limit arrives every 100 ms, so there is a delay inside the loop. A proportional loop with gain near or above 1 and a sample of delay overshoots on every step.

"Fall at once, rise at 10%/s" does not remove this; it turns it into a sawtooth. Each sag crossing drops torque in one step, recovery takes seconds, then it repeats. That is torque jumping up and down with a period of a few seconds. It is the behaviour Berkeley's code comment anticipates [FEB-4].

Recommendation, in order of preference:

- Taper on an **IR-compensated** minimum cell voltage (measured voltage + current x cell resistance), which is the Orion approach [ORION p.13]. Sag then cancels to first order and the loop gain falls to the resistance estimation error.
- If loaded voltage must be used, widen the taper so that full-current sag is well under half the taper width, and low-pass the minimum cell voltage before the taper.
- Keep a hard, fast, loaded-voltage cut as a separate backstop below the taper, as Orion's backup step and Berkeley's second threshold do [ORION p.13][FEB-1]. A protection that acts rarely does not need to be smooth.
- Measure pack resistance on the car early. Berkeley's 62 V at 65 A is the kind of number that decides this [FEB-2].

**C. "Falls immediately" passes every noisy low sample straight to torque.**

One low minimum-cell reading drops the limit at once and costs up to several seconds of reduced power while it climbs back at 10%/s. Recommendation: give the fall a finite rate too (fast, for example full scale in a few hundred milliseconds), or require the low reading to persist for two BMS samples. Temperature needs no fast path at all; it moves slowly.

**D. A 100 ms limit frame becomes 100 ms torque steps.**

The control step is 10 ms; the ceiling input changes every 100 ms. Recommendation: slew the received limit inside the VCU so a change is spread over the frame period. Texas's rule for a different limiter is the right shape: reductions take effect quickly, only recovery is slow [LH-4].

**E. 76 kW is the least margin of any published setpoint, and we have no Energy Meter to calibrate against.**

Published setpoints are 70 kW electrical and 62 to 63 kW mechanical [LH-1][WAT-1][HT-1]. Texas saw about 10% spread in their feed-forward ratio and about 4% p99 excess over target [LH-2][LH-5]. 76 kW leaves 5.3% for the sum of: feed-forward error before the trim converges, current sensor gain error, voltage measurement error, and the difference between our measurement point and the Energy Meter's. A steady error larger than that is a violation after 100 ms.

Recommendation: write the error budget down term by term; start at 70 to 72 kW; raise it only with logged evidence. Keep the learned trim across pulls so each pull starts corrected [LH-4].

**F. Do not put a computed limit in 0x0C0 bytes 6-7.**

Zero means "use EEPROM default", not zero [CM p.90]. Do the limiting in the torque command, as Texas, Berkeley and Waterloo do, and leave the field at 0 or a fixed constant.

### 8.3 Anti-oscillation measures to adopt

1. Keep the fast path free of measured power. Ceiling from speed only, every 10 ms.
2. Integral-only trim. No proportional term on measured power.
3. Update the trim once per new current frame, with the measured interval, clamped. Not once per control step.
4. Integrate only while the ceiling is binding; upward only when the inverter is tracking (8.2 A).
5. Keep the trim gain low. For an integrator around a pure delay Td, the loop is stable for `Ki x Td < pi/2` and well damped at roughly a fifth of that. With Td near 50 ms (20 ms frame, 10 ms step, 3 ms inverter loop, rate limiter) that means Ki of a few per second at most. Texas uses 2 per second [LH-1]. A time constant of 0.5 to 1 s is a sensible start.
6. Make the trim multiplicative on efficiency rather than additive in watts, so it stays valid when the BMS lowers the power ceiling.
7. On loss of the current measurement, drop to the fallback at once but keep any downward trim already learned. On recovery, rise slowly.
8. Combine the ceiling with the driver request by `min()` only, and skip P/omega below a speed floor of 10 to 15 rad/s [LH-1][FEB-1].
9. Check the inverter's `Torque_Rate_Limit` setting. During wheelspin the ceiling falls as fast as speed rises (`dT/dt = -T x (domega/dt) / omega`). If the inverter's rate limit is slower than that, delivered power overshoots the ceiling for the duration. Confirm on the bench whether the rate limit applies to decreasing torque.
10. Optional independent backstop: enable 0x202 and send a fixed or slowly varying current that corresponds to just above our own ceiling, after allowing for the efficiency it ignores [CM p.97]. If used, freeze our integrator whenever `INV_BMS_Limiting_Motor_Torque` is set.
11. Prove it the way Texas did before the car exists: a host closed-loop test with a longitudinal car model and a deliberate efficiency error, asserting peak power, settled power and maximum one-step torque change [LH-6]. Add a pack model with series resistance so the sag loop in 8.2 B is exercised too.

## 9. What remains unverified

- The MIT thesis body: PI gains, rates, anti-windup, test results [MIT-1].
- The second MIT blog post (88 kW against a predicted 65 kW; negative torque overshoot).
- The Cal Poly senior project (quadratic penalty above 60 kW).
- Any team's measured performance against the 100 ms and 500 ms tests on real Energy Meter data. None found.
- Any measured sag-recovery limit cycle on a car. Only a code comment anticipates it [FEB-4].
- Whether the Cascadia torque rate limit acts on decreasing torque, and its default value.
- Whether torque-capability limiting and the motor hot-spot model behave as described when the motor is an EMRAX rather than a catalog motor.
- Motor and inverter count for Waterloo; motor type for Texas and Kennesaw State.
- The Texas trim limiter on hardware. Their own note says simulation only [LH-5].
- FSAE forum and FSG forum posts: searches returned nothing usable.
- Our own pack resistance, which decides how serious 8.2 B is.

## Sources

Cascadia Motion

- [CM] BorgWarner Portland (formerly Cascadia Motion), *CM Software Users Manual*, document 0A-0163-04. Local copy `Datasheets/CM Software Users Manual.pdf` (untracked). Printed page numbers.

Rules and BMS

- [RULES] *Formula SAE Rules 2027*, EV.3.1.3, EV.3.3.1, EV.3.4.1, p.93. Local copy `Datasheets/FSAE_Rules_2027_V1-2.pdf`.
- [ORION] Ewert Energy Systems, *Orion BMS 2 Operation Manual*, rev 1.0, "How the BMS Calculates Current Limits", p.12-14. https://www.orionbms.com/manuals/pdf/orionbms2_operational_manual.pdf

Texas (Longhorn Racing Electric), current repository, commit ed05b957

- [LH-1] Limiter and parameters. https://github.com/LonghornRacingElectric/lhre/blob/ed05b95762db763ef9788dc6f0573483345491c9/boards/VCU/App/control/torque.cpp#L12-L71 and https://github.com/LonghornRacingElectric/lhre/blob/ed05b95762db763ef9788dc6f0573483345491c9/boards/VCU/App/config/params.hpp#L136-L153
- [LH-2] Commit message explaining why the PI was replaced. https://github.com/LonghornRacingElectric/lhre/commit/f49962eddd03cd22ec5744685aba08b4e7cf847e
- [LH-3] Open-circuit voltage estimate. https://github.com/LonghornRacingElectric/lhre/blob/ed05b95762db763ef9788dc6f0573483345491c9/boards/VCU/App/control/battery.cpp#L16-L24 and params.hpp L167-L169 at the same commit.
- [LH-4] VCU README, power limit and wheel-slip paragraphs. https://github.com/LonghornRacingElectric/lhre/blob/ed05b95762db763ef9788dc6f0573483345491c9/boards/VCU/README.md#L209-L222
- [LH-5] Pull request 182 description. https://github.com/LonghornRacingElectric/lhre/pull/182
- [LH-6] Closed-loop test. https://github.com/LonghornRacingElectric/lhre/blob/ed05b95762db763ef9788dc6f0573483345491c9/boards/VCU/App/test/app/closed_loop_test.cpp#L116-L127
- [LH-7] Derate reported but not applied. params.hpp L139-L140 at the same commit.

Texas, 2026 car repository, commit d981057b

- [LH26-1] PI limiter and gains. https://github.com/LonghornRacingElectric/lhre-2026/blob/d981057b8830f5f8be9c8b70a206dd2345748cd6/VCU/model/components/PowerLimit.c#L16-L39 and https://github.com/LonghornRacingElectric/lhre-2026/blob/d981057b8830f5f8be9c8b70a206dd2345748cd6/VCU/firmware/Core/Inc/params/default_params.h#L76-L79
- [LH26-2] Torque map and cell-voltage derate. https://github.com/LonghornRacingElectric/lhre-2026/blob/d981057b8830f5f8be9c8b70a206dd2345748cd6/VCU/model/components/TorqueMap.c#L67-L85

Berkeley (Formula Electric Berkeley)

- [FEB-1] Maximum torque and backstop, SN5, commit 0b7bffeb. https://github.com/Formula-Electric-Berkeley/FEB_FIRMWARE_SN5/blob/0b7bffeb9cbb3c6e8c16cede7e8d1d06efa15197/PCU/Core/User/Src/FEB_RMS.c#L187-L247 and L361 of the same file.
- [FEB-2] Voltage-based current derate. Same file, L110-L177.
- [FEB-3] SN4, fixed 400 V, commit 81abb1a8. https://github.com/Formula-Electric-Berkeley/FEB_Firmware_SN4/blob/81abb1a856d08e97dd23f35912d8b7a304d78cfd/PCU/Core/Src/FEB_CAN_RMS.c#L210-L231
- [FEB-4] Configuration and the oscillation warning. https://github.com/Formula-Electric-Berkeley/FEB_FIRMWARE_SN5/blob/0b7bffeb9cbb3c6e8c16cede7e8d1d06efa15197/PCU/Core/User/Inc/FEB_RMS_Config.h#L49-L132

Waterloo (UWaterloo Formula Electric), commit 0007939c

- [WAT-1] https://github.com/UWaterloo-Formula-Electric/firmware/blob/0007939cad49b0df4b4ba9c9cae93b20bfa900d7/vcu/Src/motorController.c#L164-L170 and L252; limit value at https://github.com/UWaterloo-Formula-Electric/firmware/blob/0007939cad49b0df4b4ba9c9cae93b20bfa900d7/vcu/Inc/motorController.h#L48-L51
- [WAT-2] Torque limit field. motorController.c L262-L263 at the same commit.
- [WAT-3] Endurance controller: "per motor" comment and anti-windup. https://github.com/UWaterloo-Formula-Electric/firmware/blob/0007939cad49b0df4b4ba9c9cae93b20bfa900d7/vcu/Src/endurance_mode.c#L26-L106

Kennesaw State (KSU Motorsports)

- [KSU-1] 0x202 used as the power limiter, commit a0e5aaaa. https://github.com/KSU-MS/KS6e-VCU-firmware/blob/a0e5aaaa5e71ec2b83e8c7bbfc0bd6f6f2df9ced/src/inverter.cpp#L257-L285 ; call site src/state_machine.cpp L569; 10 ms timer src/main.cpp L26; cap include/parameters.hpp L55.
- [KSU-2] Commented-out exponential decay, commit 852a201f. https://github.com/KSU-MS/VCU/blob/852a201fe22ac4255d3ff1f8c85fbb70b902c025/src/inverter.cpp#L98-L125

Georgia Tech (HyTech Racing), commit 6afeb7b6

- [HT-1] https://github.com/hytech-racing/MCU/blob/6afeb7b610453a6bd9c1598e12cac6e61a4a7e8a/lib/systems/include/TorqueControllerMux.tpp#L138-L170 ; default in TorqueControllerMux.h L56; configuration in src/main.cpp L242-L243 and L264.

MIT Motorsports

- [MIT-1] A. J. Melendez, *Design of a power limiter for an FSAE electric racecar*, S.B. thesis, MIT, 2019. Abstract only. https://dspace.mit.edu/handle/1721.1/123279
- [MIT-2] C. Vorbach, "Going Fast, But Not Too Fast", 14 Oct 2019. https://charliea0.github.io/2019/10/14/Going_Less_Zoom/

UC Irvine (Anteater Electric Racing)

- [AER-1] Firmware documentation, "power limits". https://23dcb982.aer-documentation.pages.dev/firmware/ccm/power_limits
