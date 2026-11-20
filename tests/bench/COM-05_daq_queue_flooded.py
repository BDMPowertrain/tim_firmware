#!/usr/bin/env python3
"""COM-05 DAQ send queue flooded (docs/spec/bench-test-plan.md), Rig A,
both images. The CAN interface holds the DAQ bus with frames that win
arbitration for 60 s a run, so the TIM's drop-oldest send queue fills;
the run counts as flooded only if the TIM's DAQ frame rate fell by half.
The Control bus must not notice (daq_isolation.py).

    python tests/bench/COM-05_daq_queue_flooded.py --board TIM-07 --commit 3c8cb03a --shell COM9
"""
import sys

from daq_isolation import run_case

if __name__ == "__main__":
    sys.exit(run_case("flood"))
