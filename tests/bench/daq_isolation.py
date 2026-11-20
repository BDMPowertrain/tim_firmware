"""DAQ bus isolation, shared by COM-04 (DAQ bus shorted) and COM-05 (DAQ
send queue flooded), docs/spec/bench-test-plan.md. Rig A, run on both
images: with #69 the BMS queues up to 15 DAQ frames each 100 ms, close to
its 16-deep send queue (issue 70).

Each run holds the stimulus for 60 s while the Control bus is recorded.
Pass, each run: no control-step Overrun (`step`), no Control bus frame
skipped for a full send slot (`bus`), and on the recording no frame late
(a gap over 1.5 periods) and no counter that fails to step by one
(benchlib.judge_control_bus). The time recorded is the worst gap of the
image's status frame; its limit is 1.5 periods, 30 ms.
"""
from __future__ import annotations

import threading
import time

import benchlib as bl
import rig as r

HOLD_S = 60.0
STATUS_LIMIT_MS = 20.0 * bl.LATE_FACTOR
FLOOD_ID = 0x001  # 0x000-0x07F is empty in the ID plan, and wins arbitration
COUNT_S = 5.0     # how long the DAQ frame rate is counted, before and during


def _daq_rate(rig: r.Rig, start: float, end: float) -> float:
    """Frames a second the TIM sent on the DAQ bus between two bus times."""
    n = sum(1 for f in rig.daq.since(start) if f.t <= end and f.can_id != FLOOD_ID)
    return n / (end - start)


class Flood:
    """Sends 64-byte FD frames back to back on the DAQ bus, above every ID
    the TIM sends, so its drop-oldest send queue fills."""

    def __init__(self, rig: r.Rig):
        self.rig = rig
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self) -> None:
        msg = self.rig.can.Message(arbitration_id=FLOOD_ID, is_extended_id=False, is_fd=True,
                                   bitrate_switch=True, data=bytes(64))
        while not self.stop.is_set():
            try:
                self.rig.daq_bus.send(msg, timeout=0.01)
            except self.rig.can.CanError:
                pass  # the adapter's own queue is full: that is the point

    def __enter__(self) -> "Flood":
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop.set()
        self.thread.join()


def run_case(stimulus: str) -> int:
    if stimulus == "short":
        args = r.parser("COM-04", "DAQ bus shorted").parse_args()
    else:
        args = r.parser("COM-05", "DAQ send queue flooded", daq=True).parse_args()
    rig = r.Rig(args)
    runs = []
    for n in range(args.runs):
        rig.wait_running()
        overruns, skipped = rig.overruns(), rig.tx_skipped()
        notes = []
        ok = True
        if stimulus == "short":
            rig.operator.do("Close the switch that shorts the DAQ bus (CAN2 CANH to CANL).")
            start = rig.control.now()
            time.sleep(HOLD_S)
            end = rig.control.now()
            rig.operator.do("Open the DAQ bus short.")
        else:
            # Both windows on the Control bus's clock: the TIM's DAQ frames
            # may stop altogether. Both buses are on one adapter, one clock.
            time.sleep(COUNT_S)
            with Flood(rig):
                start = rig.control.now()
                time.sleep(HOLD_S)
                end = rig.control.now()
            quiet = _daq_rate(rig, start - COUNT_S, start)
            busy = _daq_rate(rig, end - COUNT_S, end)
            notes.append(f"TIM DAQ frames/s {quiet:.0f} before, {busy:.0f} flooded")
            if busy >= quiet / 2:
                ok = False
                notes.append("not flooded: the adapter did not hold the bus")
        frames = [f for f in rig.control.since(start) if f.t <= end]
        judged = bl.judge_control_bus(frames, rig.image)
        overruns_after, skipped_after = rig.overruns(), rig.tx_skipped()
        if overruns is None or overruns_after != overruns:
            ok = False
            notes.append(f"Overruns {overruns} -> {overruns_after}")
        if skipped is None or skipped_after != skipped:
            ok = False
            notes.append(f"tx_skipped {skipped} -> {skipped_after}")
        if not judged.clean:
            ok = False
            notes.append(f"{len(judged.late)} late, {len(judged.skipped)} skipped on the bus")
        worst = judged.worst_gap_ms.get(rig.status_id)
        runs.append(bl.Run(worst, ok, "; ".join(notes)))
        print(f"run {n + 1}: worst status gap {worst} ms, {'ok' if ok else 'FAILED'} "
              f"{runs[-1].note}")
    return rig.finish("no late or skipped frame (status gap 30 ms)", STATUS_LIMIT_MS, runs)
