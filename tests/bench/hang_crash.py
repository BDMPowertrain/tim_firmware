"""The timing shared by COM-01 (hang command) and COM-02 (crash command),
docs/spec/bench-test-plan.md. Neither has an outside stimulus, so both are
timed from the last status frame on the bus (overstating by up to one
period).

VCU: on the bus alone. The last 0x0C0 before the reset to the first after
it is at most 99 ms (the inverter's own timeout); no 0x0C0 after the
command carries Enable=1, and the first after the reset carries Enable=0.
On Rig A the VCU is in GLV_ON and never sends Enable=1, so only the time
and the first frame after the reset prove anything there; with Enable=1
before the command (Ready to Drive) the bench commands are refused.

BMS: BMS_OK never rises on Rig A (no chain), so the end edge is the reset
pulse on NRST, where every output stops being driven (ADR 0012). The
scope gives the time from the end of the last BMS_Status to NRST low; the
budget is 100 ms. BMS-21 times BMS_OK itself on Rig B.
"""
from __future__ import annotations

import benchlib as bl
import rig as r

VCU_BUDGET_MS = 99.0
BMS_BUDGET_MS = 100.0


def run_case(command: str, check_crash_record: bool) -> int:
    title = "Hang command" if command == "hang" else "Crash command"
    case = "COM-01" if command == "hang" else "COM-02"
    args = r.parser(case, title).parse_args()
    rig = r.Rig(args)
    if rig.image == "bms":
        rig.operator.say("Scope: channel 1 on CAN1 (CANH, or the transceiver's TXD), channel 2 "
                         "on NRST, normal trigger on NRST falling.")
        rig.operator.do(r.DEBUG_BIT_PRECONDITION)
    runs = []
    for n in range(args.runs):
        rig.wait_running()
        mark = rig.control.now()
        rig.shell.send(f"{command} confirm")
        gap = rig.wait_reset(mark, timeout_s=10.0)
        if gap is None:
            runs.append(bl.Run(None, False, "no reset seen on the bus"))
            continue
        notes = []
        ok = True
        if rig.image == "vcu":
            frames = rig.control.since(mark)
            inv = bl.reset_gap(frames, rig.inv_command_id, mark, rig.inv_period_s)
            if inv is None:
                runs.append(bl.Run(None, False, "no 0x0C0 after the reset"))
                continue
            time_ms = round((inv[1] - inv[0]) * 1000.0, 1)
            commands = [f for f in frames if f.can_id == rig.inv_command_id]
            enabled = [f for f in commands if rig.decode(f)["InverterEnable"] == 1]
            if enabled:
                ok = False
                notes.append(f"{len(enabled)} 0x0C0 with Enable=1 after the command")
            first = next(f for f in commands if f.t >= inv[1])
            if rig.decode(first)["InverterEnable"] != 0:
                ok = False
                notes.append("the first 0x0C0 after the reset carries Enable=1")
        else:
            time_ms = rig.operator.ms("Scope: end of the last BMS_Status frame to NRST low")
        if check_crash_record:
            rig.settled_status(gap[1])
            text = rig.shell.command("lastreset")
            cause, record = bl.parse_last_reset(text), bl.parse_crash_record(text)
            crumb = bl.parse_breadcrumb(text)
            notes.append(f"lastreset: {cause}; crash record: {record}; breadcrumb: {crumb}")
            if (cause != "software" or record is None or not record.startswith("crash command")
                    or crumb is None):
                ok = False
        runs.append(bl.Run(time_ms, ok, "; ".join(notes)))
        print(f"run {n + 1}: {time_ms} ms {'; '.join(notes)}")
    if rig.image == "bms":
        return rig.finish("100 ms", BMS_BUDGET_MS, runs)
    return rig.finish("99 ms", VCU_BUDGET_MS, runs)
