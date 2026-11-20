#!/usr/bin/env python3
"""COM-15 Event flood (docs/spec/bench-test-plan.md), Rig A, both images.
Needs the ST-Link (OpenOCD reads the Event log while the board runs).

Each run, with the guard's burst refilled first (65 s quiet): 300 Events
asked for from the bus, one a control step (rig.EventStimulus: about 6 s
on the VCU, 9 s on the BMS), then 3 s to settle. The Events the run
added are read from bank 2.
Pass, each run: the burst was used (at least 64 Events written), no
stretch holds more than the guard lets through (a burst of 64 plus one a
second, benchlib.flood_excess), and an "N events dropped" Event was
written. The time recorded is the closest two Events after the burst, in
ms: about 1000 under the flood, for the record.

    python tests/bench/COM-15_event_flood.py --board TIM-07 --commit 3c8cb03a
"""
import sys
import time

import benchlib as bl
import rig as r

REFILL_S = 65.0
EVENTS = 300


def main() -> int:
    args = r.parser("COM-15", "Event flood", debugger=True).parse_args()
    rig = r.Rig(args)
    ocd = r.OpenOcd(args.openocd)
    stim = r.EventStimulus(rig)
    runs = []
    for n in range(args.runs):
        rig.operator.say(f"run {n + 1}: {REFILL_S:.0f} s for the flood guard to refill")
        time.sleep(REFILL_S)
        rig.wait_running()
        before = ocd.bank2()
        stim.send(EVENTS)
        if stim.failed:
            sys.exit(f"The Event stimulus {stim.failed}")
        time.sleep(3.0)
        added = r.new_events(before, ocd.bank2())
        admitted = [e for e in added if e.code != bl.EVENT_EVENTS_DROPPED]
        excess = bl.flood_excess(added)
        dropped = any(e.code == bl.EVENT_EVENTS_DROPPED for e in added)
        spacing = bl.spacing_after_burst(added)
        ok = len(admitted) >= bl.EVENT_FLOOD_BURST and not excess and dropped
        note = (f"{len(admitted)} Events written, dropped report {'yes' if dropped else 'NO'}"
                + (f", {len(excess)} stretches over the guard" if excess else ""))
        runs.append(bl.Run(spacing, ok, note))
        print(f"run {n + 1}: {'ok' if ok else 'FAILED'} {note}, closest after the burst "
              f"{spacing} ms")
    rig.notes.append(r.DEBUG_BIT_WARNING)
    return rig.finish("burst of 64, then one a second", None, runs)


if __name__ == "__main__":
    sys.exit(main())
