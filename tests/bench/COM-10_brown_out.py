#!/usr/bin/env python3
"""COM-10 Brown-out reset (docs/spec/bench-test-plan.md), Rig A, both
images, on a provisioned board (`tim_options` says PROVISIONED).

The variable supply dipped below 2.8 V and back. "BOR flag set" is a
Power-on reset (ADR 0011): the BMS starts normally (not FAULT_HELD), the
VCU ends in GLV_ON, and neither reports LastResetNotPowerOn. On the BMS
the scope confirms MCU_BMS_OK never went high (rig.reset_case). The
thresholds themselves are Bench measurement M-16 (issue 73).

    python tests/bench/COM-10_brown_out.py --board TIM-07 --commit 3c8cb03a
"""
import sys

import rig as r


def stimulus(rig: r.Rig) -> None:
    rig.operator.do("Lower the TIM's supply to 2.5 V for about 1 s, then back to its normal "
                    "voltage.")


if __name__ == "__main__":
    sys.exit(r.reset_case(r.Rig(r.parser("COM-10", "Brown-out reset").parse_args()),
                          stimulus, held=False, not_power_on=False,
                          budget="BMS normal start, VCU GLV_ON"))
