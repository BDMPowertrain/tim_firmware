---
status: accepted
---

# Pedal calibration survives a reflash and is proven by a pedal check

Flashing a new VCU image keeps the stored pedal and brake calibration. Before the first start procedure after a flash, and after a new calibration, the VCU requires one pedal check with the Tractive System off: pedal released (both APPS below 5%), pressed to the floor (both above 95%), released again, the two sensors within 10 points throughout, and the brake reading released and then engaged. Until it passes, the start procedure is refused. The pass is written as an event carrying the image's build ID and the parameter save counter; a check is due whenever no such event matches the running image and the loaded parameters. If the event log has lost it, the check is asked for again.

The existing checks catch one sensor wrong relative to the other, a pedal that reads pressed at rest and a sensor outside its range. They do not catch both sensors wrong in the same direction (full torque at half pedal). The pedal check tests the stored numbers against the real pedal.

## Considered options

- **Always recalibrate after a flash** (calibration stamped with the build ID). Rejected: every flash needs a laptop and time at the pedal box, including in the paddock, and a fresh calibration is still not tested against full travel.
- **Trust the stored calibration.** Rejected: leaves the same-direction error open.
- **A pedal check at every power-on.** Not chosen: it adds a step to every start. Revisit if the pedal box proves to move between runs.

## Consequences

- "Calibrated" means: a valid parameter copy holds every end point; each sensor has minimum below maximum, a span above a minimum size and end points inside its electrically valid window; and the stored calibration revision equals the image's.
- The image carries a calibration revision number. Whoever changes anything between a sensor and its number (ADC channel, resolution, scaling, filtering) bumps it in the same commit, which makes every stored calibration "not calibrated".
- End points are captured by a guided shell command (pedal released, then fully pressed; both sensors must move together) and are never typed. The same command exists for the brake sensor. `param dump` prints calibration as a record only.
- The pedal check is the one place where behaviour depends on the event log's contents; a lost log costs one extra pedal press and nothing else.
