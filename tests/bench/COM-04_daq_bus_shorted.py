#!/usr/bin/env python3
"""COM-04 DAQ bus shorted (docs/spec/bench-test-plan.md), Rig A, both
images. A switch shorts CAN2's CANH to CANL for 60 s a run; the DAQ bus
goes bus-off and retries once a second, and the Control bus must not
notice (daq_isolation.py).

    python tests/bench/COM-04_daq_bus_shorted.py --board TIM-07 --commit 3c8cb03a --shell COM9
"""
import sys

from daq_isolation import run_case

if __name__ == "__main__":
    sys.exit(run_case("short"))
