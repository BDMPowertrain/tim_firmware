#!/usr/bin/env python3
"""COM-08 NRST pin reset (docs/spec/bench-test-plan.md), Rig A, both images.

A switch pulls NRST to ground and lets it go. The BMS ends in FAULT_HELD,
the VCU in GLV_ON, and both report LastResetNotPowerOn. On the BMS the
scope confirms MCU_BMS_OK never went high (rig.reset_case). Power-cycle
the board first if a debugger has been connected since: with the ST-Link
attached, a pin reset reads as power-on on the BMS (ADR 0011).

    python tests/bench/COM-08_nrst_pin_reset.py --board TIM-07 --commit 3c8cb03a
"""
import sys

import rig as r


def stimulus(rig: r.Rig) -> None:
    rig.operator.do("Press and release the NRST switch (NRST to ground).")


if __name__ == "__main__":
    sys.exit(r.reset_case(r.Rig(r.parser("COM-08", "NRST pin reset").parse_args()),
                          stimulus, held=True, not_power_on=True,
                          budget="BMS FAULT_HELD, VCU GLV_ON"))
