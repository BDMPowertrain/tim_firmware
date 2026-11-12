# Power limiter: our design against other teams, and the case that 72 kW is safe

Research note, 2026-11-12, for issue #31. Planning input only; no firmware follows from this file directly.

Our design is the resolution of #21 and ADRs 0017 and 0018. The earlier survey of other teams is `docs/research/power-limiter-control.md` on branch `research/power-limiter-control` (called "the first pass" below). This note reuses its sources and re-reads the primary ones at the same pinned commits.

How to read this note:

- Every claim carries a source tag such as [CM p.97] or [LH-1]. Tags are resolved in "Sources" at the end.
- **Assumption** marks a number we chose because no source gives it. **Analysis** marks our own reasoning. **Unverified** marks a claim we could not check.
- Cascadia manual page numbers are printed footer numbers (PDF page = printed + 1). Rules pages are the printed "Page n of 147".

## 1. Summary

1. Our structure (feed-forward P/omega ceiling, slow one-sided trim on measured power) is the one Texas moved to after its PI hunted [LH-2], and the one MIT ended up with after testing each half alone [MIT-1 p.20]. Our feed-forward cap, 72 kW x 0.85 = 61.2 kW mechanical, is almost the same as Waterloo's 62 kW and Georgia Tech's 63 kW mechanical defaults [WAT-1][HT-1].
2. We are more conservative than every team read on loss of data (torque cut on BMS_Power timeout), on measurement integrity (a measured current, CRC, counter), and in writing a budget down at all. Only MIT also sets a requirement from its own sensor's accuracy and the Energy Meter's sample alignment [MIT-1 p.9-11].
3. **72 kW holds on paper, but the margin is spent on efficiency, not on the current sensor.** The 100 ms test is shorter than the trim can act, so the feed-forward alone must stay under 80 kW. That needs the real effective efficiency to stay at or above about **0.79** wherever the ceiling binds (5.3). Once the trim has converged, a current sensor reading up to about **8%** low still passes. If a 5% efficiency step can arrive before the trim reacts, the sensor must be within about **3.8%**. A sensor with **2% or better** total error at 130 to 210 A leaves room for both.
4. Two transients can beat the margin: wheelspin while the speed used in the ceiling is up to 27 ms old (5.5, up to 86 kW for 100 ms with a light driveline), and a trim too slow to rescue a feed-forward error (5.6). Predicting speed forward fixes the first; only margin covers the second.
5. MIT's thesis was retrieved this time. It records a disqualification from endurance for a power violation, an efficiency that shifted between home testing and competition weather, and power spikes from wheel slip in a P/omega limiter [MIT-1 p.6-8, p.17-19]. The Cal Poly project is still unreachable (HTTP 403).

## 2. Our design in one paragraph

Every 10 ms: torque ceiling = power ceiling x 0.85 / motor speed (inverter 0x0A5); torque sent = min(driver request, ceiling). Power ceiling = min(72 kW, BMS discharge limit x pack voltage). An integral-only trim, 0 to -15%, updates once per new BMS_Power frame (20 ms) while the limiter binds, is kept between pulls, starts at -15% after BMS_Power was invalid, and crosses its range in about 0.5 s. Measured power = BMS pack current x BMS pack voltage from one sweep. A BMS_Power timeout (100 ms) cuts torque. The BMS tapers the discharge limit on sag-compensated minimum cell voltage (100% at 3.0 V to 20% at 2.7 V) and maximum cell temperature (100% at 50 C to 20% at 57 C), falling at up to 200%/s and rising at up to 10%/s, and sends the same limit, unscaled, to the inverter on 0x202 as a backup. The Energy Meter is logged, never in the control path. [#21][ADR-0017][ADR-0018]

## 3. Side-by-side comparison

"None found" means we read the code at the pinned commit and found nothing, not that the team has nothing.

### 3.1 Power limiter

| | **Us** | **Texas (current)** | **Berkeley SN5** | **Waterloo** | **Kennesaw State** | **Georgia Tech** | **MIT 2019** |
|---|---|---|---|---|---|---|---|
| Structure | FF P/omega ceiling, min() with request | FF P/omega ceiling, min() with torque map [LH-1 L63-70] | P/omega cap on max torque, pedal scales it [FEB-1] | P/omega cap, min() [WAT-1] | Inverter does P/omega from 0x202 [KSU-1] | Scales the four torques so sum T x omega <= limit [HT-1] | FF mechanical limiter plus PI on battery power [MIT-1 p.15, p.20] |
| Target | 72 kW electrical | 70 kW electrical [LH-1 params L145] | 60 A x pack V, about 30 kW; a pack limit, not the rule [FEB-4] | 62 kW mechanical [WAT-1] | 80 kW; frame only sent when set below 80 kW [KSU-1 L270] | 63 kW mech default, 51.5 kW configured [HT-1] | 20 kW in the best reported test [MIT-1 p.21-22] |
| Efficiency in FF | 0.85 constant, chosen low | Ratio 0.95 = V x I / (torque feedback x omega); logged p50 0.858, p99 0.990 [LH-1][LH-6 L31-32] | None | None | None; the inverter ignores it [CM p.97] | None | None at first, then an Id/Iq map [MIT-1 p.15-16, p.19-20] |
| Mechanical cap equivalent | 61.2 kW | Not comparable (relative to the inverter's torque estimate) | about 30 kW | 62 kW | V x I limit | 63 / 51.5 kW | n/a |
| Speed source | Motor speed 0x0A5 | Motor rpm [LH-1 L41] | Motor speed [FEB-1] | Inverter motor speed [WAT-1] | Inside the inverter | Measured motor speeds [HT-1] | Wheel speed, first version [MIT-1 p.15, p.17] |
| Low speed | Floor about 100 rpm, divide-by-zero only | Cap off below 10 rad/s [LH-1] | Cap off below 15 rad/s [FEB-4] | max(0, ...) only | 525 rpm floor, 10-pole motor [CM p.97] | None found | Not stated |
| Measurement | BMS current x cell-sum V, one sweep, 20 ms, CRC + counter | Inverter Vdc x Idc; Idc is calculated, not measured [LH-1 L40][CM p.144] | IVT V in the cap, IVT I in a backstop [FEB-1] | Inverter V x I, logged only [WAT-1] | Inverter Vdc, 10 ms timer [KSU-1] | None for the limit; reads the Energy Meter current for state of charge [HT-2] | CAN current sensor, about 10 ms, 0.4% I, 0.6% V [MIT-1 p.9] |
| Trim / feedback | Integral, down only, 0 to -15%, once per frame, only while binding, kept, starts at -15% after invalid | Integral, down only, 0 to 10 kW (14%), Ki 2 /s, once per sample, gap clamped 0.2 s, only while binding, kept [LH-1 L43-61] | Torque x (60 A / I) when I > 60 A [FEB-1] | None | None | PID option, switched off [HT-1] | PI, Kp 50-75, Ki 10-30; both overshot [MIT-1 p.21] |
| Rate limit or filter on ceiling | None | None | None | None | Inverter torque rate limit only | None | Cap made non-increasing within a pull ("tCAP") [MIT-1 p.18] |
| Backup | 0x202 = BMS discharge limit, unscaled | None found | IVT current backstop | 0x0C0 limit field 0 = EEPROM limits [WAT-2] | 0x202 is the limiter | None found | n/a |
| Lost measurement | Torque cut (100 ms) | Trim holds; cap uses speed only | "Don't limit torque without real data" [FEB-2] | n/a | n/a | n/a | n/a |
| Proof | Host plant test, bench scripts, car logs, one check of both rule windows (planned) | Closed-loop host test, no slip, no delays modelled; "Not run on hardware" [LH-5][LH-6] | "Bench-tune" comments only [FEB-4] | None found | None found | None found | Stands, alley and lot tests, more than 5 runs per figure [MIT-1 p.16-21] |

### 3.2 Battery derating

| | **Us** | **Texas** | **Berkeley** | **Waterloo** | **Kennesaw State** | **Georgia Tech** |
|---|---|---|---|---|---|---|
| Where | BMS, through the discharge limit | VCU | VCU | None found in the VCU | None: Orion DCL only used as an OK flag [KSU-3] | VCU |
| Voltage input | Min cell V + I x R (sag-compensated) | Open-circuit estimate, sampled only after 1 s under 5 A, EMA alpha 0.005 [LH-3] | Loaded pack V (IVT, else inverter) [FEB-2] | - | - | Loaded min cell V, EMA alpha 0.8 [HT-3] |
| Voltage taper | 3.0 V 100% to 2.7 V 20% | 3.2 V 1.0 to 2.8 V 0 [LH-1 L15-24] | 460 V 100% to 410 V 16.7% [FEB-2] | - | - | 3.5 V 100% to 3.2 V 20% [HT-3] |
| Temperature taper | 50 C 100% to 57 C 20% | None found | None found | - | Torque "not OK" at 60 C [KSU-3] | 50 C 100% to 58 C 20% [HT-3] |
| Rate limit | Fall 200%/s, rise 10%/s | EMA only | None | - | - | EMA only |
| Applied? | Yes (design) | No: "derate is reported but not applied" [LH-7] | Yes | - | - | No: only printed at this commit [HT-3] |
| Coarse second step | UV fault at 2.505 V on raw voltage | None | 220 to 150 Nm below 420 V [FEB-1] | - | - | None |

Waterloo's endurance energy controller computes a discharge current limit, but at this commit it is stored and nothing reads it [WAT-3][WAT-4].

## 4. Difference by difference

"More conservative" means less likely to break the rule or hurt the pack, at a cost in power or availability.

| Difference | Us against them | Why |
|---|---|---|
| Target 72 kW electrical | **Less** conservative than Texas (70 kW electrical); **equal** to Waterloo and Georgia Tech in mechanical terms (61.2 against 62-63 kW); **more** than Kennesaw (80 kW) | A mechanical cap gives electrical = mechanical / real efficiency: 62 kW / 0.85 = 72.9 kW. Waterloo and Georgia Tech run what we run, minus the trim. |
| Efficiency 0.85, not measured | **Less** than Texas, who set its ratio from car logs at the high end of the spread [LH-2][LH-5] | Ours is a guess. MIT's map spans 0.2-0.9 [MIT-1 p.20] and shifted with weather and state of charge [MIT-1 p.8-9]. |
| Measured BMS current, CRC, counter | **More** than Texas and Waterloo, who use the inverter's calculated current [CM p.144] | A measured current in the pack loop is the current the Energy Meter sees. |
| Down-only trim, 15% | Same shape as Texas (14%) | An upward trim winds up when the inverter derates by itself [first pass 8.2 A]. |
| Trim starts at -15% after invalid | **More** than anyone | Costs up to 15% power for about 0.5 s after a dropout. |
| Torque cut on BMS_Power timeout | **More** than all; Berkeley drops its voltage derate when data is stale [FEB-2] | Costs availability: a CAN gap over 100 ms is a torque cut. |
| No speed prediction or slip guard in the ceiling | **Same** as everyone except MIT, **less** than MIT's non-increasing cap | MIT saw slip spikes [MIT-1 p.17-18]. See 5.5. |
| Sag-compensated taper in the BMS | **More** careful than Berkeley and Georgia Tech (loaded voltage); the Orion method [ORION p.13]; Texas's rest-only estimate is more isolated from sag, but cannot see a cell collapsing under load | Removes the sag loop to first order; depends on a measured resistance. |
| Temperature taper 50-57 C | Nearly the same as Georgia Tech (50-58 C) [HT-3] | Both end 2-3 C short of a 60 C limit. |
| 0x202 backup = discharge limit only | **Less** than Kennesaw for the 80 kW rule (its 0x202 is the power limiter); **more** than the rest (none found) | Ours backs up the pack limit, not the rule. See R3. |
| Written error budget | **More** than all but MIT [MIT-1 p.9-11] | |
| Rule-window check on logs | **More** than all found | MIT judged its own sensor with a safety factor of two on sample count [MIT-1 p.11]; we should borrow that (R2). |

**Things others do that we do not, worth considering:**

1. Calibrate the feed-forward ratio from car logs before trusting it (Texas [LH-2][LH-5]).
2. Judge our own logs against a threshold below 80 kW and a run shorter than 100 ms, to allow for sensor error and Energy Meter sample alignment (MIT: own reading 79.2 kW, at most 4 consecutive 10 ms samples over [MIT-1 p.9-11]).
3. Read the Energy Meter's 0x10D frame on the car (Georgia Tech reads it for state of charge [HT-2]). We already log it. The meter is in the car for the whole competition [EM p.3, EM.2.2.2], so practice and early-event logs give a direct BMS-against-meter comparison before endurance.
4. Guard the ceiling against fast motor speed changes (MIT's non-increasing cap within a pull [MIT-1 p.18]).

**Things we do that nobody has shown working on a car:**

1. A slow, one-sided trim on measured power. Texas's is simulation only [LH-5].
2. A sag-compensated discharge-limit taper in a team-built BMS. Orion does it commercially [ORION p.13]; no student code found.
3. A fall-fast, rise-slow discharge-limit rate (200%/s, 10%/s).
4. 0x202 as a backup beside a VCU limiter. Kennesaw uses it as the only limiter [KSU-1].
5. Any limiter judged against both rule windows on Energy Meter data. No team publishes this.

## 5. What we consider safe: first-draft error budget for 72 kW

### 5.1 The two rule tests

- A violation is power over 80 kW "Continuously for 100 ms or more" or "After a moving average over 500 ms is applied" [RULES EV.3.4.1, p.93].
- Penalty: each Acceleration, Skidpad or Autocross run with a violation disqualifies the team's best run; in Endurance, 60 s per violation [RULES EV.3.5.1-2, p.93].
- 72 kW leaves 80 / 72 = **1.111**, an 11.1% gain on the target, for every error together.

**Analysis.** For a burst at P_b above a 72 kW base, the 500 ms average passes 80 kW once (P_b - 72) x duration > 8 kW x 0.5 s = 4 kJ:

| Burst level | Moving-average violation if longer than |
|---|---|
| 90 kW | 222 ms |
| 100 kW | 143 ms |
| 112 kW | 100 ms |
| 130 kW | 69 ms |

So the 100 ms test binds for bursts up to about 112 kW, and the moving average binds above that. A **steady** bias over 80 kW fails both. Millisecond spikes from the inverter current loop fail neither.

### 5.2 Terms

| Term | Value used | Direction | Basis |
|---|---|---|---|
| e_I, current sensor gain + offset at 130-210 A | **unknown; solved for** | either | Sensor not chosen (#29). At 72 kW the pack carries 134 A at 538 V, 188 A at 384 V, 208 A at 346 V (128 cells x 4.2 / 3.0 / 2.7 V [#14]). |
| e_V, cell-sum voltage | 0.1% low, worst case | unfavourable | 128 cells x -3.5 / +2.6 mV ADC error [#14] = -0.45 / +0.33 V on 384 V or more. |
| Location: Energy Meter against BMS | 0.3% in our favour (**Assumption**) | favourable | The meter is in series in one pole and senses the other pole, on the switched side of the isolation relays [EM p.7]. It sees the cell sum less the drop in interconnects, fuse, relays and cables, so it reads less power than we do. The current is the same series current. 0.3% is about 1.2 V at 190 A, about 6 mOhm. Measure it. |
| e_EM, Energy Meter accuracy | 1%, reading high (**Assumption**) | unfavourable | The manual gives no accuracy, resolution or sample rate, only that 0x10D is sent every 20 ms [EM p.10]. MIT saw a mean of 8.33 ms between points in 2018 meter data [MIT-1 p.10]. |
| Speed delay Td | 15 ms typical, 27 ms worst (**Assumption**) | unfavourable while speeding up | 3 ms broadcast loop, so a 10 ms rate gives up to 12 ms [CM p.82]; + up to 10 ms until our step uses it; + command processed within 3 ms [CM p.90]; + about 2 ms torque response (**Assumption**). |
| Feed-forward efficiency spread | solved for | unfavourable when below 0.85 | Texas logged ratio p50 0.858, p99 0.990, a 15% spread [LH-6 L31-32]; the first pass's "about 10%" understated it. MIT's map spans 0.2-0.9 [MIT-1 p.20]. |
| Trim residual and ripple | 1% (**Assumption**) | unfavourable | Integral loop on a 20 ms sample. |
| Trim delay | about 55 ms (**Assumption**) | | 20 ms frame + up to 20 ms sweep age + 10 ms step + 3 ms inverter + response. |

The ceiling uses no measured power, so **e_I enters only through the trim**. That splits the budget into cases.

### 5.3 Case 1: feed-forward alone (what the 100 ms test sees)

The trim needs hundreds of milliseconds (5.6), so for the first 100 ms at any new operating point the feed-forward alone sets power:

P_EM = 72 x (0.85 / eta) x (1 + speed bias) x (1 + e_EM) x (1 - location)

With the 2.6% speed bias of normal acceleration at 15 m/s (5.5): other factors = 1.026 x 1.01 x 0.997 = 1.033.

| Real effective efficiency eta | P_EM |
|---|---|
| 0.85 | 74.4 kW |
| 0.80 | 79.0 kW |
| 0.775 | 81.6 kW |
| 0.75 | 84.3 kW |

Solving P_EM = 80: **eta_min = 0.85 x 72 x 1.033 / 80 = 0.790**. Without the speed bias it is 0.770.

"Effective efficiency" here is T_commanded x omega / (V x I). It includes any gap between commanded and produced torque, which MIT found real on a Rinehart [MIT-2]. **72 kW is safe only if the effective efficiency stays at or above about 0.79 wherever the ceiling binds.** That is 7% under our constant. Field weakening at high speed, a hot motor and a sagging pack all push it down [MIT-1 p.8][MIT-2]. This is the term we know least, and the one that most needs car logs.

### 5.4 Case 2: trim converged (steady pull)

When the feed-forward over-delivers, the trim holds measured power at 72 kW, so the true power is 72 / (1 - e_I) for a sensor reading low by e_I:

P_EM = 72 / (1 - e_I) x (1 + e_V) x (1 + e_EM) x (1 - location) x (1 + ripple) = 72 / (1 - e_I) x 1.018

| Sensor reads low by | P_EM |
|---|---|
| 0.5% | 73.7 kW |
| 1% | 74.0 kW |
| 2% | 74.8 kW |
| 5% | 77.2 kW |

Solving for 80 kW: **e_I <= 1 - 72 x 1.018 / 80 = 8.4%.**

**Case 3: converged, then the feed-forward error steps by d before the trim reacts** (entering field weakening, a hotter motor). P_EM = 72 x 1.018 x (1 + d) / (1 - e_I):

| Step d | Largest sensor error that still passes |
|---|---|
| 3% | 5.6% |
| 5% | 3.8% |
| 8% | 1.0% |

**For #29:** a sensor with **2% or better total error (gain, offset and temperature) from 130 to 210 A** leaves room for a 5% feed-forward step plus the other terms. A 1 A offset is 0.5-0.7% at these currents, so offset counts.

### 5.5 Transients against the 100 ms window

**Normal acceleration.** The ceiling uses a speed Td old. Power = ceiling x omega_now / omega_old, so the error is (d omega/dt / omega) x Td. With grip, d omega/dt / omega = a / v = P_mech / (m v^2). With m = 280 kg (**Assumption**) and P_mech = 61.2 kW:

| Speed | d omega/dt / omega | Bias at 15 ms | Bias at 27 ms |
|---|---|---|---|
| 10 m/s | 2.19 /s | 3.3% | 5.9% |
| 15 m/s | 0.97 /s | 1.5% | 2.6% |
| 20 m/s | 0.55 /s | 0.8% | 1.5% |

It lasts as long as the car accelerates, so it is a bias, not a spike. It is largest just above the speed where the ceiling starts to bind. Predicting speed forward by Td cancels it.

**Wheelspin.** If the tyres lose 30 kW of traction, the excess accelerates the rotating inertia J reflected to the motor: J x omega x d omega/dt = 30 kW. Averaged over 100 ms with Td = 27 ms (J and the 30 kW are **Assumptions**):

| J (kg m^2) | omega at start | Average bias | Power |
|---|---|---|---|
| 0.03 | 250 rad/s | 19% | 86.0 kW |
| 0.05 | 250 rad/s | 14% | 82.4 kW |
| 0.05 | 350 rad/s | 9% | 78.6 kW |
| 0.08 | 350 rad/s | 6% | 76.6 kW |

**A 100 ms wheelspin just above the onset speed can break the 100 ms test** unless something cuts the spin or the ceiling looks ahead. MIT saw slip-driven spikes of this kind on stands [MIT-1 p.17]. When grip returns the motor slows and the error goes the safe way.

**Pedal stamp.** Stamping above the onset speed sends the ceiling at once. It is computed from the current speed, so the limiter adds no overshoot. What is left is the inverter's own torque response (milliseconds) and its torque rate limit, which slows the rise [CM p.121]. Not a 100 ms risk.

**Torque rate limit.** While speed rises the ceiling must fall at T x (d omega/dt) / omega: 200 Nm x 6.7 /s = 1340 Nm/s, about 4 Nm per 3 ms, in the spin case. The inverter sets its limit "in terms of torque increment every 3 milliseconds" [CM p.121]. Whether it also slows decreases is still **Unverified**; if it does, it must be at least this fast.

### 5.6 How fast the trim can rescue an excess

**Analysis.** Treating the trim as a first-order lag with time constant 1/Ki after about 55 ms of delay, the time above 80 kW starting from P0 is 55 ms + (1/Ki) x ln((P0 - 72) / 8):

| Ki | Ki x Td (stable below 1.57) | From 81 kW | From 82 kW | From 85 kW |
|---|---|---|---|---|
| 2 /s (Texas; about our "0.5 s") | 0.11 | 114 ms | 167 ms | 298 ms |
| 5 /s | 0.28 | 79 ms | 100 ms | 152 ms |
| 10 /s | 0.55 | 67 ms | 77 ms | 104 ms |

At the planned rate the trim **cannot** pull back a feed-forward error over 80 kW inside 100 ms. It does protect the 500 ms average and every later lap. The budget must not count on it for the 100 ms test, so 5.3 is the deciding case. A trim at 5-10 /s is still well damped and would rescue an excess of 1-2 kW.

### 5.7 Result

| Case | 72 kW holds if |
|---|---|
| Feed-forward, first 100 ms | effective efficiency >= 0.79 everywhere the ceiling binds |
| Trim converged | current sensor error <= 8.4% |
| Converged, then a 5% step | current sensor error <= 3.8% |
| Wheelspin, 100 ms | reflected inertia high enough, or the spin is cut, or speed is predicted (table in 5.5) |

72 kW is defensible on paper **if** the efficiency assumption holds and the sensor is 2% or better. Neither is measured yet.

## 6. When to move the target

**It could move up, one step at a time and only on logged evidence, when all of these hold:**

1. Car logs give the effective efficiency over the whole binding region, and its 1st percentile is above 0.85. Each 0.01 of measured headroom is worth about 0.9 kW.
2. The current sensor is calibrated on the car against a reference to 1% or better.
3. At competition, BMS_Power and the Energy Meter's 0x10D agree within 1% over practice and early events. The meter is installed for the whole competition [EM p.3], so this is possible before endurance.
4. The speed used in the ceiling is predicted forward, or wheelspin is cut by traction control.
5. The rule-window check on car logs passes with the stricter threshold of R2.

MIT estimated, for its 2018 car, about 30 s of endurance time lost at 75 kW and about 70 s at 70 kW, read from its Figure 5 [MIT-1 p.13]. That is one car, read off a plot: a guide to what a few kilowatts are worth, nothing more.

**It should move down when any of these holds:**

1. Logged effective efficiency falls below 0.79 anywhere the ceiling binds: high speed, hot motor, low state of charge, a hot day. MIT's violation came from exactly that shift [MIT-1 p.8-9].
2. The trim sits at its -15% bound in normal running: the feed-forward error is bigger than the trim can hold.
3. The chosen sensor's total error is above 4%, or it cannot be calibrated.
4. Logs show the Energy Meter reading more than 1% above BMS_Power.
5. Logs show 100 ms wheelspin just above the onset speed and there is no traction control.
6. The Energy Meter Violation bit in 0x40D ever sets.

## 7. Recommended changes (each is a decision for the human)

- **R1 (decision for the human): predict motor speed forward by the known delay** in the ceiling, omega + (d omega/dt) x Td, increasing direction only. Removes the acceleration bias (2.6% at 15 m/s) and most of the wheelspin overshoot (5.5). One difference per new speed frame.
- **R2 (decision for the human): make the rule-window check stricter than the rule** when it judges our own BMS_Power logs: threshold 80 kW x (1 - e_I - e_EM), about 78 kW with a 1% sensor, and fail on 4 or more consecutive 20 ms samples over it (MIT's sample-alignment argument with a safety factor of two [MIT-1 p.10-11]).
- **R3 (decision for the human): consider an independent 80 kW backup on 0x202.** Today 0x202 carries only the discharge limit [ADR-0017]. Sending min(discharge limit, about 63 kW / inverter Vdc) would make the inverter hold electrical power at or below 80 kW down to an efficiency of 0.79 with no VCU code involved, the way Kennesaw uses it [KSU-1]. Costs: it depends on BMS firmware, fails open after 1 s [CM p.97], and if set at or below our ceiling it becomes the limit that binds and the trim never runs (the trim only integrates while the VCU ceiling binds).
- **R4 (decision for the human): raise the trim rate to about 5 /s** from about 2 /s. Ki x Td stays near 0.28, well damped, and the trim can then pull back a 1-2 kW excess inside 100 ms (5.6). Check it in the host test with a 55 ms delay.
- **R5 (decision for the human): log effective efficiency and the trim value from the first drive**, and plan the efficiency table now rather than "later". The constant is the largest unmeasured term.
- **R6 (decision for the human): ask #29 for a sensor with 2% or better total error at 130-210 A**, offset included, and calibrate it on the car.
- **R7 (decision for the human): plan a BMS_Power against 0x10D comparison** on the first runs at competition, with a rule agreed in advance to lower the target if they disagree by more than 1%.

## 8. Retry of sources the first pass could not read

- **MIT thesis [MIT-1]: read.** The handle page's download links returned 429, but the DSpace REST endpoint (`/server/api/core/bitstreams/<id>/content`) served the PDF. Used above: the disqualification (p.6-8), sensor accuracy and the 79.2 kW requirement (p.9-11), slip spikes and the non-increasing cap (p.17-18), field results with PI gains (p.20-22). It reports no test at 80 kW: "All of the work thus far has been tested to a particular set point" [MIT-1 p.22].
- **Mismatch between the MIT sources.** The blog says the 2018 car was over "for about 5 milliseconds off the line" [MIT-2]. The thesis shows the violation at the start of endurance and a long debate with judges over the meter's validity and resolution [MIT-1 p.7]. A 5 ms excess fails neither current rule window, so one of the two is imprecise. **Unverified** which.
- **The second MIT blog post (88 kW against 65 kW predicted): still unreachable.** The blog's index lists no such post [MIT-2 index].
- **Cal Poly senior project: still unreachable.** The likely document is A. Saha, "Vehicle Control Unit", Cal Poly CPE senior project 301, 2019 [CP-1]. Digital Commons and the CORE mirror returned 403 and the Scribd copy reset the connection. The quadratic penalty above 60 kW stays **Unverified**, and we are not certain this is the right document.

## 9. What remains unverified

- Our effective efficiency, anywhere. 0.85 and the 0.79 floor are not measured.
- The Energy Meter's accuracy, sample rate and interpolation. The 2026 manual states none [EM].
- The current sensor and its accuracy (#29).
- Reflected rotating inertia, vehicle mass and onset speed used in 5.5.
- Whether the Cascadia torque rate limit slows decreasing torque, and its default.
- The resistance between the cell taps and the Energy Meter's sense point.
- Any team's limiter measured against both rule windows on Energy Meter data. Still none found.
- The Cal Poly project and the second MIT blog post.
- Motor and motor count for Waterloo, Texas and Kennesaw State (unchanged from the first pass).

## Sources

Rules and Energy Meter

- [RULES] *Formula SAE Rules 2027*, Version 1.0: EV.3.1.3 p.92; EV.3.2, EV.3.3.1, EV.3.4.1, EV.3.5 p.92-93. Local copy `Datasheets/FSAE_Rules_2027_V1-2.pdf` (untracked).
- [EM] *Formula SAE Energy Meter User Manual*, 27 Oct 2026: EM.2.2 p.3; tractive system wiring p.7; CAN messages p.10-11. No accuracy figure anywhere in its 16 pages. Local copy `Datasheets/EM_User_Manual_10.27.2026kz.pdf` (untracked).

Inverter and BMS

- [CM] *CM Software Users Manual*, document 0A-0163-04: torque capability p.35; CAN broadcast 3 ms loop p.82; command processed every 3 ms p.90; 0x202 BMS limit p.96-97; torque rate limit parameter p.121; "The calculated DC Bus current" p.144. Local copy `Datasheets/CM Software Users Manual.pdf` (untracked).
- [ORION] Orion BMS 2 Operation Manual, p.12-14. https://www.orionbms.com/manuals/pdf/orionbms2_operational_manual.pdf

This project

- [#21] Resolution comment on issue 21, BDMPowertrain/tim_firmware.
- [#14] Resolution comment on issue 14 (128 cells, cell ADC error -3.5 / +2.6 mV, 20 ms sweep).
- [ADR-0017], [ADR-0018] `docs/adr/0017-power-limiter-is-feed-forward-with-a-down-only-trim.md`, `docs/adr/0018-bms-owns-derating-on-sag-compensated-cell-voltage.md` (not yet committed when this was written).

Texas (Longhorn Racing Electric), commit ed05b957

- [LH-1] https://github.com/LonghornRacingElectric/lhre/blob/ed05b95762db763ef9788dc6f0573483345491c9/boards/VCU/App/control/torque.cpp#L12-L71 ; parameters https://github.com/LonghornRacingElectric/lhre/blob/ed05b95762db763ef9788dc6f0573483345491c9/boards/VCU/App/config/params.hpp#L127-L153
- [LH-2] https://github.com/LonghornRacingElectric/lhre/commit/f49962eddd03cd22ec5744685aba08b4e7cf847e
- [LH-3] params.hpp L166-L169 and boards/VCU/App/control/battery.cpp L16-L24 at the same commit.
- [LH-5] https://github.com/LonghornRacingElectric/lhre/pull/182 ("p99 reported power is 72.9 kW at a 70 kW target"; "Not run on hardware").
- [LH-6] https://github.com/LonghornRacingElectric/lhre/blob/ed05b95762db763ef9788dc6f0573483345491c9/boards/VCU/App/test/app/closed_loop_test.cpp#L20-L127 (plant has no slip and no delays; ratio p50 0.858 and p99 0.990 at L31-L32).
- [LH-7] params.hpp L139-L140 at the same commit.

Berkeley, FEB_FIRMWARE_SN5 commit 0b7bffeb

- [FEB-1] https://github.com/Formula-Electric-Berkeley/FEB_FIRMWARE_SN5/blob/0b7bffeb9cbb3c6e8c16cede7e8d1d06efa15197/PCU/Core/User/Src/FEB_RMS.c#L187-L247 and L361.
- [FEB-2] Same file, L110-L177 ("Don't limit torque without real data" in that range).
- [FEB-4] https://github.com/Formula-Electric-Berkeley/FEB_FIRMWARE_SN5/blob/0b7bffeb9cbb3c6e8c16cede7e8d1d06efa15197/PCU/Core/User/Inc/FEB_RMS_Config.h#L49-L132

Waterloo, commit 0007939c

- [WAT-1] https://github.com/UWaterloo-Formula-Electric/firmware/blob/0007939cad49b0df4b4ba9c9cae93b20bfa900d7/vcu/Src/motorController.c#L164-L170 and L252; limit in vcu/Inc/motorController.h L48-L51.
- [WAT-2] motorController.c L262-L263, same commit.
- [WAT-3] https://github.com/UWaterloo-Formula-Electric/firmware/blob/0007939cad49b0df4b4ba9c9cae93b20bfa900d7/vcu/Src/endurance_mode.c#L84-L138
- [WAT-4] motorController.c L74-L78 stores the limit; nothing else in vcu/ reads `DischargeCurrentLimit` at this commit.

Kennesaw State, KS6e-VCU-firmware commit a0e5aaaa

- [KSU-1] https://github.com/KSU-MS/KS6e-VCU-firmware/blob/a0e5aaaa5e71ec2b83e8c7bbfc0bd6f6f2df9ced/src/inverter.cpp#L257-L285 ; call site src/state_machine.cpp L569; cap include/parameters.hpp L55.
- [KSU-3] https://github.com/KSU-MS/KS6e-VCU-firmware/blob/a0e5aaaa5e71ec2b83e8c7bbfc0bd6f6f2df9ced/src/accumulator.cpp#L49-L62

Georgia Tech (HyTech), MCU commit 6afeb7b6

- [HT-1] https://github.com/hytech-racing/MCU/blob/6afeb7b610453a6bd9c1598e12cac6e61a4a7e8a/lib/systems/include/TorqueControllerMux.tpp#L138-L170 ; default lib/systems/include/TorqueControllerMux.h L56; configuration src/main.cpp L238-L264.
- [HT-2] https://github.com/hytech-racing/MCU/blob/6afeb7b610453a6bd9c1598e12cac6e61a4a7e8a/lib/interfaces/src/AMSInterface.cpp#L95 , L125, L172-L173 (Energy Meter current used for state of charge).
- [HT-3] Same file L66-L77 and L180-L207; alphas in lib/interfaces/include/AMSInterface.h L19-L20; only use at src/main.cpp L500.

MIT Motorsports

- [MIT-1] A. J. Melendez, *Design of a power limiter for an FSAE electric racecar*, S.B. thesis, MIT, June 2019. https://dspace.mit.edu/handle/1721.1/123279 ; PDF served at https://dspace.mit.edu/server/api/core/bitstreams/58d52afc-8537-4d2d-8816-fb054b2dd27b/content . Printed page numbers.
- [MIT-2] C. Vorbach, "Going Fast, But Not Too Fast", 14 Oct 2019. https://charliea0.github.io/2019/10/14/Going_Less_Zoom/ ; index https://charliea0.github.io/

Cal Poly

- [CP-1] A. Saha, "Vehicle Control Unit", Cal Poly Computer Engineering senior project 301, 2019. https://digitalcommons.calpoly.edu/cpesp/301/ (PDF returned 403).
