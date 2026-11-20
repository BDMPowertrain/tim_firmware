#!/usr/bin/env python3
"""COM-11 Step timing while bank 2 is erased (docs/spec/bench-test-plan.md),
Rig A, both images. Proves the bank split (ADR 0014): erasing and writing
bank 2 stalls nothing that runs from bank 1. Needs the ST-Link (OpenOCD
reads the Event log while the board runs) and the shell.

Each run, with the flood guard's burst refilled first (65 s quiet):
  1. `param save`: the older Parameter copy is erased and written (on the
     VCU only in GLV_ON, which a bare TIM is in).
  2. 70 Events from the bus (rig.EventStimulus, one a step: about 0.7 s
     on the VCU, 1.4 s on the BMS): the guard admits its burst of 64, a
     page's worth, so at least one Event log page is erased back to back
     with the writes. The page numbers before and after prove it.
Pass, each run: a page was opened, no Overrun (`step`), no Control bus
frame skipped for a full slot (`bus`), and no late or skipped frame on the
bus. The time recorded is the worst gap of the image's status frame, its
limit 1.5 periods (30 ms).

    python tests/bench/COM-11_erase_and_save_timing.py --board TIM-07 \\
        --commit 3c8cb03a --shell COM9
"""
import sys
import time

import benchlib as bl
import rig as r

REFILL_S = 65.0  # the burst of 64 refills at one a second
EVENTS = 70
STATUS_LIMIT_MS = 20.0 * bl.LATE_FACTOR


def main() -> int:
    args = r.parser("COM-11", "Step timing while bank 2 is erased", debugger=True).parse_args()
    rig = r.Rig(args)
    ocd = r.OpenOcd(args.openocd)
    stim = r.EventStimulus(rig)
    runs = []
    for n in range(args.runs):
        rig.operator.say(f"run {n + 1}: {REFILL_S:.0f} s for the flood guard to refill")
        time.sleep(REFILL_S)
        rig.wait_running()
        pages_before = bl.event_log_page_numbers(ocd.bank2())
        overruns, skipped = rig.overruns(), rig.tx_skipped()
        start = rig.control.now()
        reply = rig.shell.command("param save", wait_s=2.0)
        stim.send(EVENTS)
        if stim.failed:
            sys.exit(f"The Event stimulus {stim.failed}")
        time.sleep(3.0)
        end = rig.control.now()
        pages_after = bl.event_log_page_numbers(ocd.bank2())
        overruns_after, skipped_after = rig.overruns(), rig.tx_skipped()

        frames = [f for f in rig.control.since(start) if f.t <= end]
        judged = bl.judge_control_bus(frames, rig.layout)
        opened = max(pages_after, default=0) - max(pages_before, default=0)
        saved = "param: saved" in reply
        notes = [f"pages opened {opened}", "saved" if saved else "param save not confirmed"]
        ok = opened >= 1 and saved
        if overruns is None or overruns_after != overruns:
            ok = False
            notes.append(f"Overruns {overruns} -> {overruns_after}")
        if skipped is None or skipped_after != skipped:
            ok = False
            notes.append(f"tx_skipped {skipped} -> {skipped_after}")
        if not judged.clean:
            ok = False
            notes.append(f"{len(judged.late)} late, {len(judged.skipped)} skipped on the bus")
        worst = judged.worst_gap_ms.get(rig.status_id)
        runs.append(bl.Run(worst, ok, "; ".join(notes)))
        print(f"run {n + 1}: worst status gap {worst} ms, {'ok' if ok else 'FAILED'} "
              f"{runs[-1].note}")
    rig.notes.append(r.DEBUG_BIT_WARNING)
    return rig.finish("no Overrun, no late or skipped frame (status gap 30 ms)",
                      STATUS_LIMIT_MS, runs)


if __name__ == "__main__":
    sys.exit(main())
