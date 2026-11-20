#!/usr/bin/env python3
"""COM-03 Three Overruns in a second (docs/spec/bench-test-plan.md), Rig A,
either image. Needs the ST-Link and the image's ELF with debug information.

Stimulus. The image has no fault-injection code (spec #36 story 66), and a
debugger halt cannot cause an Overrun: the RTOS tick and the cycle counter
stop with the core. So GDB stops the control step three steps running in
step_sched_next() (App/common/logic/Src/step_timing.c), the one function
that judges an Overrun, and moves its now_ms on by one period each time.
To the image each of those steps ended one period late: it counts the
Overrun, posts its Event, skips a period, and the third makes the
timing fault (ADR 0013). Housekeeping uses the same function on a 100 ms
period, so the breakpoint is on the step's own period.

Pass, each run: the step's Overrun count (`step`) rose by 3 and
  BMS: InternalActive set in BMS_Status, then cleared after 1 s clean:
       set for 1.0 to 1.1 s (BMS-11's window), plus one status period for
       where the frames fall. The time recorded is how long it was set.
  VCU: TimingFault set in VCU_Status. Its recovery needs the pedal below
       5%, which a bare TIM cannot give (its pedal sensors read a supply
       fault): VCU-16 on Rig C.

GDB must be able to write now_ms: a Debug image (-O0). If it cannot,
the run fails saying so.

    python tests/bench/COM-03_three_overruns.py --board TIM-07 --commit 3c8cb03a \\
        --shell COM9 --elf build/Debug/bms/tim.elf
"""
import sys
import time

import benchlib as bl
import rig as r

PERIOD_MS = {"bms": 20, "vcu": 10}
RECOVERY_MS = (1000.0, 1100.0 + 20.0)  # BMS-11, plus one BMS_Status period
GDB_FAILED = ("optimized out", "No symbol", "Cannot access")

GDB_SCRIPT = """\
break step_sched_next if sched->period_ms == {period}
continue
set var now_ms = now_ms + sched->period_ms
continue
set var now_ms = now_ms + sched->period_ms
continue
set var now_ms = now_ms + sched->period_ms
delete
monitor resume
detach
"""


def main() -> int:
    args = r.parser("COM-03", "Three Overruns in a second", debugger=True, elf=True).parse_args()
    rig = r.Rig(args)
    bit = "InternalActive" if rig.image == "bms" else "TimingFault"
    script = GDB_SCRIPT.format(period=PERIOD_MS[rig.image])
    runs = []
    with r.GdbSession(args.openocd, args.gdb, args.elf) as gdb:
        for n in range(args.runs):
            rig.wait_running()
            time.sleep(3.0)  # well clear of the last run's second
            before = rig.overruns()
            mark = rig.control.now()
            out = gdb.run(script)
            trouble = [line for line in out.splitlines() if any(t in line for t in GDB_FAILED)]
            if trouble:
                runs.append(bl.Run(None, False, "GDB could not inject: " + trouble[0].strip()))
                print(f"run {n + 1}: FAILED {runs[-1].note} (a Debug image is needed)")
                continue
            raised = rig.control.wait(
                lambda f: f.can_id == rig.status_id and rig.decode(f)[bit] == 1, 2.0, after=mark)
            after = rig.overruns()
            counted = None if before is None or after is None else after - before
            notes = [f"Overruns counted {counted}"]
            ok = raised is not None and counted == 3
            time_ms = None
            if raised is None:
                notes.append(f"{bit} never set")
            elif rig.image == "bms":
                cleared = rig.control.wait(
                    lambda f: f.can_id == rig.status_id and rig.decode(f)[bit] == 0, 3.0,
                    after=raised.t)
                if cleared is None:
                    ok = False
                    notes.append(f"{bit} not cleared in 3 s")
                else:
                    time_ms = round((cleared.t - raised.t) * 1000.0, 1)
                    if not RECOVERY_MS[0] <= time_ms <= RECOVERY_MS[1]:
                        ok = False
                        notes.append(f"set {time_ms} ms, outside 1.0 to 1.1 s")
            runs.append(bl.Run(time_ms, ok, "; ".join(notes)))
            print(f"run {n + 1}: {'ok' if ok else 'FAILED'} {runs[-1].note}"
                  + ("" if time_ms is None else f", {bit} set for {time_ms} ms"))
    rig.notes.append(r.DEBUG_BIT_WARNING)
    rig.operator.say(r.DEBUG_BIT_WARNING)
    return rig.finish("pass or fail", None, runs)


if __name__ == "__main__":
    sys.exit(main())
