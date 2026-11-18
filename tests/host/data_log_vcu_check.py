#!/usr/bin/env python3
"""Checks what tools/data_log_decode.py makes of the VCU sample files that
test_vcu_log writes (issue 61), then judges the decoded step logs with
tools/power_rule_windows.py, the one check of the 80 kW rule's windows.

    python data_log_vcu_check.py DECODER RULE_CHECK DIR

The expected values are worked by hand from what test_vcu_log.c put in the
files. Needs cantools (requirements.txt) for the Message database.
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


def decode(decoder: str, tdl: Path, out: Path) -> str:
    done = subprocess.run([sys.executable, decoder, str(tdl), "-o", str(out)],
                          capture_output=True, text=True)
    if done.returncode != 0:
        sys.exit(f"the decoder failed on {tdl}:\n{done.stdout}{done.stderr}")
    return done.stdout


def check_sample(decoder: str, d: Path) -> None:
    out = d / "vcu_actual"
    summary = decode(decoder, d / "vcu_log_sample.tdl", out)

    for line in ["vcu_step: 2 records, 1 lost in 1 gaps",
                 "imu: 2 records, 0 lost in 0 gaps",
                 "daq_rx: 2 records, 0 lost in 0 gaps",
                 "daq_tx: 1 records, 0 lost in 0 gaps",
                 "mailboxes: 1 records, 0 lost in 0 gaps",
                 "bad records: 0",
                 "torn tail: 0 bytes"]:
        expect(f"summary line {line!r}", line in summary.splitlines(), True)

    steps = rows(out / "vcu_log_sample.vcu_step.csv")
    expect("step rows", len(steps), 2)
    s0, s2 = steps
    for col, want in {
        "seq": "0", "uptime_ms": "2000", "state": "3", "step_us": "400",
        "sdc_closed": "1", "can_running": "1", "start_pressed": "0",
        "limiter_active": "1", "lower_ceiling": "discharge", "command_sent": "1",
        "apps1_travel_pct": "65.5", "apps1_status": "OK",
        "torque_request_nm": "150.0", "torque_nm": "120.0", "torque_ceiling_nm": "120.0",
        "power_ceiling_w": "72000", "trim_ppm": "-1000", "cuts": "0x0000",
        "command": "b004000001010000",
        "INV_Command.TorqueCommand": "120.0", "INV_Command.InverterEnable": "1",
        "INV_Command.DirectionCommand": "1",
        "BMS_Power.state": "VALID", "BMS_Power.PackCurrent": "100.0",
        "BMS_Power.PackVoltage": "500.0", "BMS_Power.DischargeLimit": "200.0",
        "bms_power_w": "50000.0",
        "EM_Measurement.state": "VALID", "EM_Measurement.Current": "120.5",
        "em_power_w": "72300.0",
        "BMS_Status.state": "NODATA", "IMD_InfoGeneral.state": "NODATA",
    }.items():
        expect(f"step 0 {col}", s0.get(col), want)
    for col, want in {
        "seq": "2", "uptime_ms": "2020", "step_count": "2", "cuts": "0x0002",
        "apps2_status": "OUT_OF_RANGE", "command_sent": "0", "command": "",
        "INV_Command.TorqueCommand": "", "BMS_Power.state": "INVALID",
        "BMS_Power.PackCurrent": "0.0", "bms_power_w": "", "em_power_w": "",
    }.items():
        expect(f"step 2 {col}", s2.get(col), want)

    imu = rows(out / "vcu_log_sample.imu.csv")
    expect("imu rows", len(imu), 2)
    expect("imu 0 az_raw", imu[0].get("az_raw"), "4098")
    expect("imu 0 az_g", imu[0].get("az_g"), "0.999912")
    expect("imu 1 gz_dps", imu[1].get("gz_dps"), "70.0")
    expect("imu 1 uptime_ms", imu[1].get("uptime_ms"), "2011")

    rx = rows(out / "vcu_log_sample.daq_rx.csv")
    expect("daq_rx rows", len(rx), 2)
    expect("daq_rx 0 id", rx[0].get("id"), "0x280")
    expect("daq_rx 0 fd", rx[0].get("fd"), "1")
    expect("daq_rx 0 name", rx[0].get("name"), "BMS_DaqStatus")
    for sig in ["OverrunSeen=1", "CardState=1", "RecordsDropped=5"]:
        expect(f"daq_rx 0 has {sig}", sig in rx[0].get("signals", "").split(";"), True)
    expect("daq_rx 1 id", rx[1].get("id"), "0x1abcdef")
    expect("daq_rx 1 extended", rx[1].get("extended"), "1")
    expect("daq_rx 1 data", rx[1].get("data"), "010203")
    expect("daq_rx 1 name", rx[1].get("name"), "")

    tx = rows(out / "vcu_log_sample.daq_tx.csv")
    expect("daq_tx 0 name", [r.get("name") for r in tx], ["VCU_DaqStatus"])


def check_no_map(decoder: str, d: Path) -> None:
    out = d / "vcu_actual"
    summary = decode(decoder, d / "vcu_log_no_map.tdl", out)
    expect("no-map summary says the mailboxes are unnamed",
           "vcu_step: no mailbox map: mailboxes named by position, not decoded"
           in summary.splitlines(), True)
    steps = rows(out / "vcu_log_no_map.vcu_step.csv")
    expect("no-map step rows", len(steps), 1)
    if steps:
        expect("no-map mailbox2.state", steps[0].get("mailbox2.state"), "VALID")
        expect("no-map mailbox2.data", steps[0].get("mailbox2.data"), "0000e80000000000")
        expect("no-map bms_power_w", steps[0].get("bms_power_w"), "")


def check_rule(decoder: str, rule_check: str, d: Path, name: str, want_exit: int) -> None:
    out = d / "vcu_actual"
    decode(decoder, d / f"{name}.tdl", out)
    done = subprocess.run([sys.executable, rule_check, str(out / f"{name}.vcu_step.csv"),
                           "--time-column", "uptime_ms", "--power-column", "em_power_w"],
                          capture_output=True, text=True)
    expect(f"rule check on {name} (exit)\n{done.stdout}{done.stderr}", done.returncode, want_exit)
    if want_exit == 1:
        expect(f"rule check on {name} names a continuous Violation",
               "continuous:" in done.stdout, True)


def main() -> int:
    decoder, rule_check, d = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    check_sample(decoder, d)
    check_no_map(decoder, d)
    check_rule(decoder, rule_check, d, "vcu_rule_pass", 0)
    check_rule(decoder, rule_check, d, "vcu_rule_fail", 1)
    for f in failures:
        print(f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
