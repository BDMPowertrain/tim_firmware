#!/usr/bin/env python3
"""Checks what tools/data_log_decode.py makes of the BMS sample file that
test_bms_log writes (issue 69): both BMS Record types, their shared sequence
numbers, and the cell temperatures from the sensors' millivolts.

    python data_log_bms_check.py DECODER DIR

The expected values are worked by hand from what test_bms_log.c put in the
file. Standard library only.
"""
from __future__ import annotations

import csv
import subprocess
import sys
from pathlib import Path

failures: list[str] = []


def expect(what: str, got, want) -> None:
    if got != want:
        failures.append(f"{what}: got {got!r}, want {want!r}")


def rows(path: Path) -> list[dict[str, str]]:
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def main() -> int:
    decoder, d = sys.argv[1], Path(sys.argv[2])
    out = d / "bms_actual"
    done = subprocess.run([sys.executable, decoder, str(d / "bms_log_sample.tdl"),
                           "-o", str(out)], capture_output=True, text=True)
    if done.returncode != 0:
        sys.exit(f"the decoder failed:\n{done.stdout}{done.stderr}")

    lines = done.stdout.splitlines()
    for line in ["file: image BMS, build 0000b115, boot 9, file 1, layout 1, opened at 4000 ms",
                 # seq 0-3 and 6 pack, 4 snapshot: 5 lost, judged over both.
                 "bms_pack: 5 records, 1 lost in 1 gaps",
                 "bms_snapshot: 1 records, numbered with bms_pack",
                 "bad records: 0",
                 "torn tail: 0 bytes"]:
        expect(f"summary line {line!r}", line in lines, True)

    # Every step's pack-level fields, the snapshot's included, in order.
    pack = rows(out / "bms_log_sample.bms_pack.csv")
    expect("pack rows", [r["seq"] for r in pack], ["0", "1", "2", "3", "4", "6"])
    expect("from a snapshot", [r["snapshot"] for r in pack], ["0", "0", "0", "0", "1", "0"])
    p0, p6 = pack[0], pack[5]
    for col, want in {
        "uptime_ms": "4000", "step_count": "100", "state": "1", "startup_ok": "1",
        "step_us": "9000", "swept": "1", "sdc_closed": "1", "nfault_ok": "1",
        "bms_ok_fb": "1", "can_running": "1", "bus_off_latched": "0", "bms_ok": "1",
        "charger_valid": "1", "current_valid": "0", "bridge_read": "1",
        "balance_command": "0", "faults_active": "0x0000", "chain_state": "RUNNING",
        "charge_state": "CHARGING", "balance_state": "1", "devices_read": "0xffff",
        "bridge_fault": "0", "cells_read": "128", "sensors_read": "127",
        "pack_voltage_v": "499.25", "min_cell_v": "3.9", "max_cell_v": "3.95",
        "min_cell": "0.0", "max_cell": "2.5", "min_temp_c": "25.0", "max_temp_c": "25.0",
        "pack_current_a": "0.0", "discharge_limit_a": "0.0", "charge_limit_a": "12.5",
        "cell_spread_mv": "50",
    }.items():
        expect(f"pack 0 {col}", p0.get(col), want)
    for col, want in {"uptime_ms": "4120", "step_count": "106", "current_valid": "1",
                      "pack_current_a": "-12.5"}.items():
        expect(f"pack 6 {col}", p6.get(col), want)

    snaps = rows(out / "bms_log_sample.bms_snapshot.csv")
    expect("snapshot rows", len(snaps), 1)
    s = snaps[0] if snaps else {}
    for col, want in {
        "seq": "4", "uptime_ms": "4080", "step_count": "104",
        "v_00_0": "3.9", "v_02_5": "3.95", "v_15_7": "3.9",
        "t_00_0": "25.0", "t_00_0_mv": "1860",
        "t_07_1": "", "t_07_1_mv": "3000",  # open: outside 1300-2440 mV
        "b_00_0": "1", "b_00_1": "0", "b_00_2": "1", "b_15_7": "0",
    }.items():
        expect(f"snapshot {col}", s.get(col), want)
    expect("snapshot columns", len(s), 3 + 4 * 128)

    for f in failures:
        print(f, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
