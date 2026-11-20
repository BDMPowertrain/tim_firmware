#!/usr/bin/env python3
"""COM-06 Watchdog reset (docs/spec/bench-test-plan.md), Rig A, both images.

`hang confirm`, then the watchdog resets the MCU. The BMS ends in
FAULT_HELD, the VCU in GLV_ON, and both report LastResetNotPowerOn. On the
BMS the scope confirms MCU_BMS_OK never went high (rig.reset_case).
Power-cycle the board first if a debugger has been connected since.

    python tests/bench/COM-06_watchdog_reset.py --board TIM-07 --commit 3c8cb03a --shell COM9
"""
import sys

import rig as r


def stimulus(rig: r.Rig) -> None:
    rig.shell.send("hang confirm")


if __name__ == "__main__":
    sys.exit(r.reset_case(r.Rig(r.parser("COM-06", "Watchdog reset").parse_args()),
                          stimulus, held=True, not_power_on=True,
                          budget="BMS FAULT_HELD, VCU GLV_ON"))
