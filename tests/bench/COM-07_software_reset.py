#!/usr/bin/env python3
"""COM-07 Software reset (docs/spec/bench-test-plan.md), Rig A, both images.

`crash confirm`: the crash routine resets the MCU. The BMS ends in
FAULT_HELD, the VCU in GLV_ON, and both report LastResetNotPowerOn. On the
BMS the scope confirms MCU_BMS_OK never went high (rig.reset_case).
Power-cycle the board first if a debugger has been connected since.

    python tests/bench/COM-07_software_reset.py --board TIM-07 --commit 3c8cb03a --shell COM9
"""
import sys

import rig as r


def stimulus(rig: r.Rig) -> None:
    rig.shell.send("crash confirm")


if __name__ == "__main__":
    sys.exit(r.reset_case(r.Rig(r.parser("COM-07", "Software reset").parse_args()),
                          stimulus, held=True, not_power_on=True,
                          budget="BMS FAULT_HELD, VCU GLV_ON"))
