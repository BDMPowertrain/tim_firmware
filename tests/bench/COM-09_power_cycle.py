#!/usr/bin/env python3
"""COM-09 Power cycle (docs/spec/bench-test-plan.md), Rig A, both images.

The supply's switch off, then on. A Power-on reset: the BMS starts
normally (not FAULT_HELD; on Rig A it then goes to FAULT for the missing
chain, which is expected), the VCU ends in GLV_ON, and neither reports
LastResetNotPowerOn. On the BMS the scope confirms MCU_BMS_OK never went
high (rig.reset_case).

    python tests/bench/COM-09_power_cycle.py --board TIM-07 --commit 3c8cb03a
"""
import sys

import rig as r


def stimulus(rig: r.Rig) -> None:
    rig.operator.do("Switch the TIM's supply off, wait 2 s, switch it on.")


if __name__ == "__main__":
    sys.exit(r.reset_case(r.Rig(r.parser("COM-09", "Power cycle").parse_args()),
                          stimulus, held=False, not_power_on=False,
                          budget="BMS normal start, VCU GLV_ON"))
