#!/usr/bin/env python3
"""COM-01 Hang command (docs/spec/bench-test-plan.md), Rig A, either image.

`hang confirm` stops the control step; the watchdog resets the MCU. See
hang_crash.py for the edges and budgets.

    python tests/bench/COM-01_hang_command.py --board TIM-07 --commit 3c8cb03a --shell COM9
"""
import sys

from hang_crash import run_case

if __name__ == "__main__":
    sys.exit(run_case("hang", check_crash_record=False))
