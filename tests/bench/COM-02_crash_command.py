#!/usr/bin/env python3
"""COM-02 Crash command (docs/spec/bench-test-plan.md), Rig A, either image.

`crash confirm` calls the crash routine, which writes the Crash record and
resets. The same edges and budgets as COM-01 (hang_crash.py); after each
reset `lastreset` must give a software reset, the Crash record
"crash command in shellTask" and a Breadcrumb.

    python tests/bench/COM-02_crash_command.py --board TIM-07 --commit 3c8cb03a --shell COM9
"""
import sys

from hang_crash import run_case

if __name__ == "__main__":
    sys.exit(run_case("crash", check_crash_record=True))
