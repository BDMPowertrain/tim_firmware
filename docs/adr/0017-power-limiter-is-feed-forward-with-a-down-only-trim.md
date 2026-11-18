---
status: accepted
---

# The power limiter is a feed-forward ceiling with a down-only trim, and the Energy Meter is never in the control path

The 80 kW rule (EV.3.3.1, EV.3.4.1) limits electrical power leaving the pack, judged on the event Energy Meter over two windows: continuously over the limit for 100 ms, and a 500 ms moving average. The VCU commands torque, so the power limiter turns a power ceiling into a torque ceiling every 10 ms control step: power ceiling x efficiency / motor speed, with efficiency a fixed, deliberately low constant (0.85). The torque sent is the lower of that and the driver's request, so the limiter can only reduce torque (EV.3.1.3). Measured pack power (BMS pack current x pack voltage, one sweep, every 20 ms) feeds only a slow integral trim that may lower the ceiling by up to 15% and never raise it.

The power ceiling is the lower of a fixed 72 kW target and the BMS discharge limit x pack voltage. The Energy Meter is logged, and its Violation bit is written to the Event log, but nothing in the torque path reads it.

## Considered options

- **A PI loop on measured power.** Rejected: the measurement is 20 ms old and the loop must catch an overshoot inside the 100 ms window. The one public team that ran this (Texas, on a Cascadia inverter) reported it "overshot and could cycle between full torque and zero" and replaced it with the design above.
- **Feed-forward alone.** Rejected as the end state: it is only as good as the efficiency guess, and other teams log about 10% spread in it. It is what remains at the trim's bound.
- **A trim that can also raise the ceiling.** Rejected: +15% would permit 87 kW, and the integrator winds up whenever the inverter delivers less than commanded (its own temperature derating, voltage limit at high speed), then overshoots when that clears.
- **The Energy Meter as the measurement.** Rejected although it is the instrument that scores us: the team cannot buy one and sees it only at competition, so a loop on it could never be tested.
- **Inverter DC voltage x its DC current.** Rejected: that current is calculated, not measured, and the frame carries no CRC or counter.

## Consequences

- A pessimistic efficiency constant costs power until someone corrects it from car logs or a dyno; the trim will not find it.
- The limiter needs the pack current sensor. A lost BMS_Power frame is a torque cut, not a reduced-power mode, because the same frame carries the discharge limit.
- 72 kW is a 10% margin held by a written error budget (sensor accuracy, sample delay, trim lag), not by measurement against the Energy Meter.
- The trim updates once per new BMS_Power frame and only while the limiter is binding (and has bound for 50 ms without a break, so a frame sampled before the torque rose cannot wind it up: issue 60), is kept between pulls, and starts at its lowest bound after BMS_Power has been invalid.
- The limiter never writes a computed value into the torque-limit bytes of the inverter command; those keep the fixed maximum-torque parameter.
- The inverter's 0x202 current limit stays a backup for the discharge limit only. It gets the same value as the VCU, unscaled, so it sits just above the limiter and does not fight it. It knows nothing of 80 kW.
- The limiter is proven by a host closed-loop test against a plant model with pack resistance, bench scripts, and car logs, all judged by one shared check of the two rule windows.
