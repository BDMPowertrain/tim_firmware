#!/usr/bin/env python3
"""COM-12 Power removed during a write (docs/spec/bench-test-plan.md),
Rig A, both images. Needs the shell and the ST-Link. Runs alternate
between the two writes (or --during picks one):

  save:  a sentinel parameter is saved at value X, set to Y, and `param
         save` sent; the power is cut at once (the two page erases take
         about 45 ms).
  event: Events are sent from the bus (rig.EventStimulus, the flood
         guard's burst of 64 is written in about 0.7 s) and the power is
         cut while they are.

Pass, each run: the board starts again; the BMS is not in FAULT_HELD (a
power loss is a Power-on reset) and the VCU is in GLV_ON; the sentinel in
force (`param dump`) is X or Y after a save run, and unchanged after an
Event run. A cut that tore a write shows as a copy good before and not
after it in `param dump` (a torn Parameter copy is skipped, not
repaired), or as a storage repaired Event after the start (a torn Event
log page is repaired, #18). The case needs at least one torn run, or it
proved nothing and fails. By hand a cut rarely lands inside 45 ms: a
relay on the supply driven at the moment of the save is the better switch
(who operates the switch is open, see the plan). Issue 72 holds the ECC
NMI checks that go with this case.

    python tests/bench/COM-12_power_loss_mid_write.py --board TIM-07 \\
        --commit 3c8cb03a --shell COM9
"""
import re
import sys
import threading
import time

import benchlib as bl
import rig as r

# The sentinel: a parameter that changes nothing on a bare TIM.
SENTINEL = {"bms": ("charge_target", 4000, 3900), "vcu": ("max_torque", 100, 200)}
EVENTS = 200


def param_dump(rig: r.Rig, name: str) -> tuple[int | None, list[str]]:
    """The sentinel's value in force and the two copies' states."""
    text = rig.shell.command("param dump", wait_s=2.0)
    value = re.search(rf"^\s*{name}\s+(-?\d+)", text, re.MULTILINE)
    copies = re.search(r"copy A (.+?), copy B (.+?);", text)
    return (int(value.group(1)) if value else None,
            list(copies.groups()) if copies else ["?", "?"])


def main() -> int:
    ap = r.parser("COM-12", "Power removed during a write", debugger=True)
    ap.add_argument("--during", choices=["both", "save", "event"], default="both")
    args = ap.parse_args()
    rig = r.Rig(args)
    ocd = r.OpenOcd(args.openocd)
    stim = r.EventStimulus(rig)
    name, x, y = SENTINEL[rig.image]
    runs = []
    torn_runs = 0
    for n in range(args.runs):
        during = args.during if args.during != "both" else ("save", "event")[n % 2]
        rig.wait_running()
        before = ocd.bank2()
        if during == "save":
            rig.shell.command(f"param set {name} {x}")
            if "param: saved" not in rig.shell.command("param save", wait_s=2.0):
                sys.exit("param save refused: on the VCU it needs GLV_ON")
            rig.shell.command(f"param set {name} {y}")
        value_before, copies_before = param_dump(rig, name)
        mark = rig.control.now()
        sender = None
        if during == "save":
            rig.shell.send("param save")
        else:
            # Sending stops by itself when the bus refuses a frame (no ACK
            # once the board is off): stim.failed says so, which is expected.
            sender = threading.Thread(target=stim.send, args=(EVENTS,), daemon=True)
            sender.start()
        rig.operator.do("CUT the TIM's power NOW, wait 2 s, restore it.")
        if sender is not None:
            sender.join()
        gap = rig.wait_reset(mark, timeout_s=15.0)
        if gap is None:
            runs.append(bl.Run(None, False, f"{during}: the board did not start again"))
            continue
        end = rig.settled_status(gap[1])
        state = rig.state_name(end) if end else "none"
        value, copies = param_dump(rig, name)
        repaired = [e for e in r.new_events(before, ocd.bank2())
                    if e.code == bl.EVENT_STORAGE_REPAIRED]
        # Torn: a copy good before the cut is not now (an erase cut leaves
        # it empty, a write cut a bad CRC or a read fault), or a page of
        # the Event log was repaired at the start.
        torn = any(a == "good" and b != "good" for a, b in zip(copies_before, copies)) \
            or bool(repaired)
        torn_runs += torn
        ok = (state != "FAULT_HELD") if rig.image == "bms" else (state == "GLV_ON")
        if during == "save":
            ok = ok and value in (x, y)
        else:
            ok = ok and value == value_before
        note = (f"{during}: ended {state}, {name} {value}, copies {copies[0]}/{copies[1]}, "
                f"repair Events {len(repaired)}{', TORN' if torn else ''}")
        runs.append(bl.Run(None, ok, note))
        print(f"run {n + 1}: {'ok' if ok else 'FAILED'} {note}")
    rig.notes.append(f"runs whose cut tore a write: {torn_runs}")
    if torn_runs == 0:
        rig.case_failures.append("no cut landed mid-write: the case proved nothing")
    return rig.finish("pass or fail", None, runs)


if __name__ == "__main__":
    sys.exit(main())
