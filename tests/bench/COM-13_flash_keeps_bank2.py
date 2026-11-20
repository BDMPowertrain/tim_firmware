#!/usr/bin/env python3
"""COM-13 A normal flash leaves bank 2 untouched (docs/spec/bench-test-plan.md),
Rig A, both images. Needs the ST-Link and the image's ELF.

Each run, with the core halted throughout so the image writes nothing in
between: dump bank 2 (0x08040000, 256 KB), flash the ELF with CLAUDE.md's
`program ... verify`, dump bank 2 again, start the board. Pass, each run:
the two dumps are identical, and bank 2 held something to keep (the
Parameters or the Event log are not blank). The time recorded is none; the
note gives the bytes that differed.

    python tests/bench/COM-13_flash_keeps_bank2.py --board TIM-07 --commit 3c8cb03a \\
        --elf build/Debug/bms/tim.elf
"""
import sys
import tempfile
from pathlib import Path

import benchlib as bl
import rig as r

BANK2_SIZE = 0x40000


def main() -> int:
    args = r.parser("COM-13", "A normal flash leaves bank 2 untouched", elf=True).parse_args()
    rig = r.Rig(args)
    ocd = r.OpenOcd(args.openocd)
    elf = Path(args.elf).resolve().as_posix()
    runs = []
    for n in range(args.runs):
        rig.wait_running()
        with tempfile.TemporaryDirectory() as d:
            before, after = Path(d) / "before.bin", Path(d) / "after.bin"
            ocd.run("reset halt",
                    f"dump_image {before.as_posix()} 0x{bl.BANK2_DUMP_ADDR:08X} 0x{BANK2_SIZE:X}",
                    f"program {elf} verify",
                    f"dump_image {after.as_posix()} 0x{bl.BANK2_DUMP_ADDR:08X} 0x{BANK2_SIZE:X}",
                    "reset run", timeout_s=300.0)
            a, b = before.read_bytes(), after.read_bytes()
        differ = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
        kept = a[:bl.BANK2_DUMP_SIZE] != b"\xff" * bl.BANK2_DUMP_SIZE
        ok = differ == 0 and kept
        note = f"{differ} bytes differ" + ("" if kept else "; bank 2 was blank: nothing proven")
        runs.append(bl.Run(None, ok, note))
        print(f"run {n + 1}: {'ok' if ok else 'FAILED'} {note}")
    rig.notes.append(r.DEBUG_BIT_WARNING)
    return rig.finish("pass or fail", None, runs)


if __name__ == "__main__":
    sys.exit(main())
