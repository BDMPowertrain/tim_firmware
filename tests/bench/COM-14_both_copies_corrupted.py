#!/usr/bin/env python3
"""COM-14 Both Parameter copies corrupted (docs/spec/bench-test-plan.md),
Rig A, both images. Needs the ST-Link. Both copies must be good first:
`param save` twice from the shell.

Each run: OpenOCD halts the core, keeps both copies, flips one bit in
each inside what its CRC covers (benchlib.corrupt_param_copy), writes them
back and starts the board. Then the kept copies are written back and the
board started again.
Pass, each run:
  - corrupted: the image runs on defaults: ParametersDefaulted set in its
    status frame; the VCU reaches GLV_ON and sends no 0x0C0 with
    Enable=1 (with defaults the start procedure is refused and maximum
    torque is 0; the refusal itself is host-tested, and VCU-19 on Rig C);
  - restored: ParametersDefaulted clear again.
The Parameters are put back each run whatever happens; if the script is
stopped mid-run, the kept copies are in the results folder as
COM-14_params_backup.bin, to write back with OpenOCD's flash write_image.

    python tests/bench/COM-14_both_copies_corrupted.py --board TIM-07 --commit 3c8cb03a
"""
import sys
from pathlib import Path

import benchlib as bl
import rig as r


def check(rig: r.Rig, mark: float, defaulted: bool) -> tuple[bool, str]:
    gap = rig.wait_reset(mark, timeout_s=15.0)
    if gap is None:
        return False, "no start seen"
    end = rig.settled_status(gap[1])
    if end is None:
        return False, "no status frame"
    state = rig.state_name(end)
    flag = rig.decode(end)["ParametersDefaulted"] == 1
    ok = flag == defaulted
    note = f"{state}, ParametersDefaulted {int(flag)}"
    if rig.image == "vcu" and defaulted:
        enabled = [f for f in rig.control.since(gap[1]) if f.can_id == rig.inv_command_id
                   and rig.decode(f)["InverterEnable"] == 1]
        ok = ok and state == "GLV_ON" and not enabled
        note += f", Enable=1 frames {len(enabled)}"
    return ok, note


def main() -> int:
    args = r.parser("COM-14", "Both Parameter copies corrupted", debugger=True).parse_args()
    rig = r.Rig(args)
    ocd = r.OpenOcd(args.openocd)
    kept = ocd.dump(bl.BANK2_DUMP_ADDR, 2 * bl.PARAMS_COPY_SIZE, halt=True)
    ocd.run("reset run")
    copies = bl.parse_param_copies(kept)
    if any(c.status != "good" for c in copies):
        sys.exit(f"Both copies must be good first (they are {copies[0].status}, "
                 f"{copies[1].status}): `param save` twice from the shell.")
    backup = Path(args.results) / "COM-14_params_backup.bin"
    backup.parent.mkdir(parents=True, exist_ok=True)
    backup.write_bytes(kept)
    corrupted = bl.corrupt_param_copy(bl.corrupt_param_copy(kept, 0), 1)
    runs = []
    for n in range(args.runs):
        rig.wait_running()
        mark = rig.control.now()
        try:
            ocd.write_params(corrupted)
            ok_bad, note_bad = check(rig, mark, defaulted=True)
        finally:
            mark = rig.control.now()
            ocd.write_params(kept)
        ok_good, note_good = check(rig, mark, defaulted=False)
        note = f"corrupted: {note_bad}; restored: {note_good}"
        runs.append(bl.Run(None, ok_bad and ok_good, note))
        print(f"run {n + 1}: {'ok' if runs[-1].ok else 'FAILED'} {note}")
    backup.unlink()
    rig.notes.append(r.DEBUG_BIT_WARNING)
    return rig.finish("pass or fail", None, runs)


if __name__ == "__main__":
    sys.exit(main())
