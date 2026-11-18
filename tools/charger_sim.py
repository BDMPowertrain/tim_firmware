#!/usr/bin/env python3
"""Plays the Elcon charger on the Control bus until the real one is on the
bench (issue 67; ADR 0009, 0019; the charger's bench findings are #33, #34).

It broadcasts CHG_Status (0x18FF50E5) every 1 s and obeys CHG_Request
(0x1806E5F4) the way the Elcon CAN Specification 2019 says the charger does:
it delivers the requested current while control is 0 (start), nothing on a
stop, and stops by itself 5 s after the last request (Operation Mode 1,
p.6). Both frames come from can/control.dbc through cantools, so a layout
is never written twice (ADR 0003). Output voltage is reported as the pack
voltage given on the command line.

Faults to try against the BMS:
  --over AMPS      deliver AMPS more than asked: over 1 A for 5 s is
                   "charger disobeys", a BMS fault
  --ignore-stop    keep delivering the last current after a stop
  --flags N        report status flags N (bits 0-4, see the DBC); no flag
                   is a BMS fault, but each change is an Event
  --silent-after S stop sending status after S seconds: the charger
                   timeout (5 s); the BMS pauses the charge, BMS_OK stays high

The bus is the Control bus at 500 kbit/s (the BMS's only rate, #33). Needs
cantools and python-can (both in requirements.txt) and a
python-can adapter, for example:

    python tools/charger_sim.py --interface pcan --channel PCAN_USBBUS1
    python tools/charger_sim.py --interface slcan --channel COM7 --over 1.5

Each request heard and each status sent is printed, one line each.
"""
from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import cantools

DBC = Path(__file__).resolve().parent.parent / "can" / "control.dbc"
STATUS_PERIOD_S = 1.0
CHARGER_STOPS_S = 5.0  # Elcon p.6: stops 5 s after the last request
CONTROL_START = 0


@dataclass
class Charger:
    """What the charger does with the requests it has heard."""

    over_a: float = 0.0
    ignore_stop: bool = False
    asked_a: float = 0.0
    last_request_s: float | None = None
    delivering_a: float = 0.0

    def request(self, now_s: float, current_a: float, control: int) -> None:
        self.last_request_s = now_s
        if control == CONTROL_START:
            self.asked_a = current_a
        elif not self.ignore_stop:
            self.asked_a = 0.0

    def output_a(self, now_s: float) -> float:
        if self.last_request_s is None:
            return 0.0
        if now_s - self.last_request_s > CHARGER_STOPS_S and not self.ignore_stop:
            return 0.0
        return self.asked_a + self.over_a


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--interface", required=True, help="python-can interface, e.g. pcan, slcan")
    ap.add_argument("--channel", required=True, help="python-can channel, e.g. PCAN_USBBUS1, COM7")
    ap.add_argument("--bitrate", type=int, default=500000)
    ap.add_argument("--pack-volts", type=float, default=480.0, help="output voltage to report")
    ap.add_argument("--over", type=float, default=0.0, metavar="AMPS")
    ap.add_argument("--ignore-stop", action="store_true")
    ap.add_argument("--flags", type=lambda s: int(s, 0), default=0, metavar="N")
    ap.add_argument("--silent-after", type=float, default=None, metavar="S")
    args = ap.parse_args()

    try:
        import can
    except ImportError:
        print("python-can is not installed: pip install python-can", file=sys.stderr)
        return 2

    db = cantools.database.load_file(DBC)
    status = db.get_message_by_name("CHG_Status")
    req = db.get_message_by_name("CHG_Request")
    names = ["HardwareFailure", "OverTemperature", "InputVoltageWrong", "StartingState",
             "CommTimeout"]

    charger = Charger(over_a=args.over, ignore_stop=args.ignore_stop)
    bus = can.Bus(interface=args.interface, channel=args.channel, bitrate=args.bitrate)
    start = time.monotonic()
    next_status = start
    try:
        while True:
            now = time.monotonic()
            msg = bus.recv(timeout=max(0.0, min(0.05, next_status - now)))
            now = time.monotonic()
            if msg is not None and msg.is_extended_id and msg.arbitration_id == req.frame_id:
                r = req.decode(msg.data, decode_choices=False)
                charger.request(now - start, r["MaxChargeCurrent"], r["Control"])
                print(f"{now - start:8.1f} s  request  {r['MaxChargeVoltage']:6.1f} V "
                      f"{r['MaxChargeCurrent']:5.1f} A  "
                      f"{'start' if r['Control'] == CONTROL_START else 'stop'}")
            if now >= next_status:
                next_status += STATUS_PERIOD_S
                if args.silent_after is not None and now - start >= args.silent_after:
                    continue
                amps = charger.output_a(now - start)
                signals = {"OutputVoltage": args.pack_volts, "OutputCurrent": amps}
                signals.update({n: (args.flags >> i) & 1 for i, n in enumerate(names)})
                bus.send(can.Message(arbitration_id=status.frame_id, is_extended_id=True,
                                     data=status.encode(signals)))
                print(f"{now - start:8.1f} s  status   {args.pack_volts:6.1f} V {amps:5.1f} A  "
                      f"flags 0x{args.flags:02x}")
    except KeyboardInterrupt:
        pass
    finally:
        bus.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
