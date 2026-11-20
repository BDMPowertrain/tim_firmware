"""Rig A for the bench scripts (issue 70, docs/spec/bench-test-plan.md):
a bare TIM, a CAN interface on both buses, the shell's UART, an ST-Link
and a person at the bench.

Everything here touches hardware, so it is proven at the bench, not by a
host test; the judging is in benchlib.py, which is. Frames are encoded and
decoded with can/control.dbc and can/daq.dbc through cantools (ADR 0003),
so no layout is written twice.

Needs tests/bench/requirements.txt: cantools and python-can, pyserial for
the shell, python-ics for a ValueCAN (python-can's "neovi" interface).
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import benchlib as bl

REPO = Path(__file__).resolve().parents[2]
CONTROL_DBC = REPO / "can" / "control.dbc"
DAQ_DBC = REPO / "can" / "daq.dbc"
RESULTS = Path(__file__).resolve().parent / "results"

SETTLE_S = 3.0  # after a start, before the state is read: the BMS on Rig A
                # leaves STARTUP for FAULT after 2 s with no chain

STATUS_FRAME = {"bms": "BMS_Status", "vcu": "VCU_Status"}
SENDER = {"bms": "BMS", "vcu": "VCU"}
COUNTER_SIGNALS = ("Counter", "RollingCounter")
EVERY_STEP_MAX_MS = 20  # sent every step or every second step: judged late


def bus_layout(db, image: str) -> bl.BusLayout:
    """The Control bus layout benchlib judges by, read from the DBC (ADR
    0003): the image's frames with a period of 20 ms or less, and every
    frame's counter signal with its number of values."""
    periods = {m.frame_id: float(m.cycle_time) for m in db.messages
               if SENDER[image] in m.senders and m.cycle_time
               and m.cycle_time <= EVERY_STEP_MAX_MS}
    counters = {}
    for m in db.messages:
        for sig in m.signals:
            if sig.name in COUNTER_SIGNALS:
                assert sig.byte_order == "little_endian", sig
                counters[m.frame_id] = (sig.start, sig.length, int(sig.maximum) + 1)
    return bl.BusLayout(periods, counters)


def parser(case: str, title: str, *, debugger: bool = False, daq: bool = False,
           elf: bool = False) -> argparse.ArgumentParser:
    """The arguments every case takes. Defaults suit a ValueCAN with the
    Control bus on network 1 and the DAQ bus on network 2."""
    ap = argparse.ArgumentParser(description=f"{case}: {title} (docs/spec/bench-test-plan.md)")
    ap.add_argument("--board", required=True, help="the board's serial, as on its label")
    ap.add_argument("--commit", required=True,
                    help="the git commit the flashed image was built from")
    ap.add_argument("--runs", type=int, default=bl.RUNS)
    ap.add_argument("--interface", default="neovi", help="python-can interface")
    ap.add_argument("--control-channel", default="1", help="python-can channel of the Control bus")
    ap.add_argument("--daq-channel", default="2", help="python-can channel of the DAQ bus")
    ap.add_argument("--shell", metavar="PORT",
                    help="serial port of the TIM's USART1 shell; without it, you are asked "
                         "to type each command")
    ap.add_argument("--results", default=str(RESULTS), help="where the result file goes")
    ap.add_argument("--scope-screenshot", help="path of the scope screenshot to name in the result")
    if debugger or elf:
        ap.add_argument("--openocd", default="openocd")
        ap.add_argument("--elf", required=elf, help="the image flashed, build/<preset>/<image>/tim.elf")
    if debugger:
        ap.add_argument("--gdb", default="arm-none-eabi-gdb")
    ap.set_defaults(case=case, title=title, daq=daq)
    return ap


# ---------------------------------------------------------------------------
# The person at the bench.

class Operator:
    """Asks for whatever the script cannot do itself: a switch, a scope
    reading (who operates the switch is open, see the plan)."""

    def do(self, what: str) -> None:
        input(f"\n>>> {what}\n    Press Enter when done. ")

    def ms(self, what: str) -> float:
        while True:
            text = input(f"\n>>> {what}\n    Type the time in ms: ").strip()
            try:
                return float(text)
            except ValueError:
                print("    Not a number.")

    def yes(self, question: str) -> bool:
        while True:
            text = input(f"\n>>> {question} [y/n] ").strip().lower()
            if text in ("y", "n"):
                return text == "y"

    def say(self, text: str) -> None:
        print(f"\n--- {text}")


# ---------------------------------------------------------------------------
# The shell (USART1, 115200 baud, a line ends on CR, the prompt is "> ").

class SerialShell:
    def __init__(self, port: str):
        import serial  # pyserial
        self.port = serial.Serial(port, 115200, timeout=0.05)

    def command(self, line: str, wait_s: float = 1.0) -> str:
        """Sends one line and returns what came back until the next prompt."""
        self.port.reset_input_buffer()
        self.port.write(line.encode("ascii") + b"\r")
        out = b""
        deadline = time.monotonic() + wait_s
        while time.monotonic() < deadline:
            out += self.port.read(256)
            if out.endswith(b"> ") and out.count(b"\n") >= 1:
                break
        return out.decode("ascii", "replace")

    def send(self, line: str) -> None:
        """Sends one line and does not wait: for a command that resets."""
        self.port.write(line.encode("ascii") + b"\r")


class ManualShell:
    """No serial port: the person types each command and, where the script
    reads the reply, pastes it back."""

    def command(self, line: str, wait_s: float = 1.0) -> str:
        print(f"\n>>> Type in the TIM shell:  {line}")
        print("    Paste its reply here, then an empty line:")
        lines = []
        while True:
            text = input()
            if not text:
                return "\n".join(lines)
            lines.append(text)

    def send(self, line: str) -> None:
        input(f"\n>>> Type in the TIM shell:  {line}\n    Press Enter here as you press it there. ")


# ---------------------------------------------------------------------------
# The buses.

class Recorder:
    """Keeps every frame one bus delivers, with the adapter's timestamp."""

    def __init__(self, bus):
        self.bus = bus
        self.frames: list[bl.Frame] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            msg = self.bus.recv(timeout=0.05)
            if msg is not None and not msg.is_error_frame:
                with self._lock:
                    self.frames.append(bl.Frame(msg.timestamp, msg.arbitration_id,
                                                bytes(msg.data)))

    def now(self) -> float:
        """The time of the newest frame: the bus's own clock."""
        with self._lock:
            return self.frames[-1].t if self.frames else 0.0

    def since(self, t: float) -> list[bl.Frame]:
        with self._lock:
            return [f for f in self.frames if f.t > t]

    def wait(self, predicate, timeout_s: float, after: float = 0.0) -> bl.Frame | None:
        """The first frame after `after` for which predicate holds, waiting
        up to timeout_s of wall time."""
        deadline = time.monotonic() + timeout_s
        seen = 0
        while time.monotonic() < deadline:
            frames = self.since(after)
            for f in frames[seen:]:
                if predicate(f):
                    return f
            seen = len(frames)
            time.sleep(0.02)
        return None

    def close(self) -> None:
        self._stop.set()
        self._thread.join()
        self.bus.shutdown()


class Rig:
    def __init__(self, args: argparse.Namespace):
        import can
        import cantools

        self.args = args
        self.can = can
        self.operator = Operator()
        self.shell = SerialShell(args.shell) if args.shell else ManualShell()
        self.control_db = cantools.database.load_file(CONTROL_DBC)
        self.daq_db = cantools.database.load_file(DAQ_DBC)
        self.control = Recorder(can.Bus(interface=args.interface, channel=args.control_channel,
                                        bitrate=500000))
        self.daq = None
        if args.daq:
            self.daq_bus = can.Bus(interface=args.interface, channel=args.daq_channel, fd=True,
                                   bitrate=500000, data_bitrate=2000000)
            self.daq = Recorder(self.daq_bus)
        self.image = self._which_image()
        status = self.control_db.get_message_by_name(STATUS_FRAME[self.image])
        self.status_id = status.frame_id
        self.status_period_s = status.cycle_time / 1000.0
        command = self.control_db.get_message_by_name("INV_Command")
        self.inv_command_id = command.frame_id
        self.inv_period_s = command.cycle_time / 1000.0
        self.layout = bus_layout(self.control_db, self.image)
        self.notes: list[str] = []
        self.case_failures: list[str] = []
        print(f"{args.case}: {args.title}; the board runs the {self.image.upper()} image")

    def _which_image(self) -> str:
        ids = {self.control_db.get_message_by_name(name).frame_id: image
               for image, name in STATUS_FRAME.items()}
        f = self.control.wait(lambda f: f.can_id in ids, 5.0)
        if f is None:
            sys.exit("No BMS_Status or VCU_Status on the Control bus in 5 s: is the TIM "
                     "running, and the bus terminated at both ends?")
        return ids[f.can_id]

    # Decoding -------------------------------------------------------------

    def decode(self, frame: bl.Frame) -> dict:
        """Any Control bus frame's signals, raw values for choices."""
        return self.control_db.decode_message(frame.can_id, frame.data, decode_choices=False)

    def state_name(self, frame: bl.Frame) -> str:
        msg = self.control_db.get_message_by_frame_id(frame.can_id)
        return str(msg.decode(frame.data)["State"])

    def wait_running(self, timeout_s: float = 10.0) -> bl.Frame:
        """Waits until status frames come and the board has settled."""
        start = self.control.now()
        f = self.control.wait(lambda f: f.can_id == self.status_id, timeout_s, after=start)
        if f is None:
            sys.exit("The board sends no status frame.")
        return f

    def wait_reset(self, mark: float, timeout_s: float = 10.0) -> tuple[float, float] | None:
        """(last status frame before, first after) the reset that follows
        mark, or None if none came in timeout_s."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            gap = bl.reset_gap(self.control.since(mark - 1.0), self.status_id, mark,
                               self.status_period_s)
            if gap is not None:
                return gap
            time.sleep(0.05)
        return None

    def settled_status(self, after: float) -> bl.Frame | None:
        """The status frame SETTLE_S after `after` (a start)."""
        time.sleep(SETTLE_S + 0.5)
        frames = [f for f in self.control.since(after) if f.can_id == self.status_id
                  and f.t <= after + SETTLE_S]
        return frames[-1] if frames else None

    # Shell readings -------------------------------------------------------

    def overruns(self) -> int | None:
        return bl.parse_overruns(self.shell.command("step"))

    def tx_skipped(self) -> int | None:
        return bl.parse_tx_skipped(self.shell.command("bus"))

    # Finishing -------------------------------------------------------------

    def finish(self, budget: str, budget_ms: float | None, runs: list[bl.Run]) -> int:
        result = bl.Result(case=self.args.case, title=self.args.title, image=self.image,
                           image_commit=self.args.commit, board=self.args.board, budget=budget,
                           budget_ms=budget_ms, runs=runs, notes=self.notes,
                           case_failures=self.case_failures,
                           scope_screenshot=self.args.scope_screenshot)
        path = bl.write_result(result, self.args.results)
        self.control.close()
        if self.daq is not None:
            self.daq.close()
        print(f"\n{result.case} {result.image.upper()}: worst "
              f"{'-' if result.worst_ms is None else f'{result.worst_ms:.1f} ms'}"
              f" (budget {budget}), {result.verdict}")
        for i, r in enumerate(runs, 1):
            if not r.ok or r.note:
                print(f"  run {i}: {'ok' if r.ok else 'FAILED'} {r.note}")
        for failure in result.case_failures:
            print(f"  case: FAILED {failure}")
        print(f"Result: {path}  (commit it)")
        return 0 if result.verdict == "PASS" else 1


# ---------------------------------------------------------------------------
# The debugger. Any OpenOCD or GDB connection sets the core's debug enable
# bit until a power cycle, and the BMS treats every reset under it as
# power-on (ADR 0011): power-cycle the board before a reset case.

DEBUG_BIT_WARNING = ("The debugger set the core's debug enable bit. Power-cycle the board "
                     "before any case that expects FAULT_HELD (COM-01, COM-02, COM-06 to COM-08).")
DEBUG_BIT_PRECONDITION = ("If OpenOCD or GDB has been connected since the board was last powered "
                          "on, power-cycle it now: under the debug enable bit the BMS takes every "
                          "reset as power-on and never holds (ADR 0011).")


class OpenOcd:
    def __init__(self, exe: str = "openocd"):
        self.exe = exe

    def run(self, *commands: str, timeout_s: float = 120.0) -> str:
        args = [self.exe, "-f", "interface/stlink-dap.cfg", "-f", "target/stm32g4x.cfg",
                "-c", "init"]
        for c in commands:
            args += ["-c", c]
        args += ["-c", "shutdown"]
        done = subprocess.run(args, capture_output=True, text=True, timeout=timeout_s)
        if done.returncode != 0:
            raise RuntimeError(f"openocd failed:\n{done.stderr[-2000:]}")
        return done.stderr

    def dump(self, addr: int, size: int, *, halt: bool = False) -> bytes:
        """Reads memory. Without halt the board keeps running."""
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "dump.bin"
            cmds = (["reset halt"] if halt else []) + [
                f"dump_image {path.as_posix()} 0x{addr:08X} 0x{size:X}"]
            self.run(*cmds)
            return path.read_bytes()

    def bank2(self) -> bytes:
        return self.dump(bl.BANK2_DUMP_ADDR, bl.BANK2_DUMP_SIZE)

    def write_params(self, blob: bytes) -> None:
        """Writes both Parameter copies (8 KB from 0x08040000) and starts
        the board."""
        size = 2 * bl.PARAMS_COPY_SIZE
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "params.bin"
            path.write_bytes(blob[:size])
            self.run("reset halt",
                     f"flash erase_address 0x{bl.BANK2_DUMP_ADDR:08X} 0x{size:X}",
                     f"flash write_image {path.as_posix()} 0x{bl.BANK2_DUMP_ADDR:08X} bin",
                     f"verify_image {path.as_posix()} 0x{bl.BANK2_DUMP_ADDR:08X} bin",
                     "reset run")


class GdbSession:
    """An OpenOCD server for the length of a `with`, and GDB scripts run
    against it in batch mode."""

    def __init__(self, openocd: str, gdb: str, elf: str):
        self.openocd, self.gdb, self.elf = openocd, gdb, elf

    def __enter__(self) -> "GdbSession":
        self.server = subprocess.Popen(
            [self.openocd, "-f", "interface/stlink-dap.cfg", "-f", "target/stm32g4x.cfg"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2.0)
        return self

    def run(self, script: str, timeout_s: float = 60.0) -> str:
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "case.gdb"
            path.write_text("set pagination off\nset confirm off\n"
                            "target extended-remote localhost:3333\n" + script + "\nquit\n")
            done = subprocess.run([self.gdb, "-nx", "-batch", "-x", str(path), self.elf],
                                  capture_output=True, text=True, timeout=timeout_s)
        return done.stdout + done.stderr

    def __exit__(self, *exc) -> None:
        self.server.terminate()
        self.server.wait(timeout=10)


# ---------------------------------------------------------------------------
# Events from outside the board, written through the real path (no
# fault-injection code, spec #36 story 66):
#   VCU: INV_FaultCodes with new fault bits, an inverter fault Event each
#        (vcu_control.c, record_inverter_fault()).
#   BMS: CHG_Status with changed flags, a charger flags Event each
#        (bms_charge.c). The charger heard puts the BMS in Charge mode
#        until a power cycle; on Rig A it has no cells to charge.
# The step reads the newest frame of its mailbox once a step, so at most
# one Event a step comes of it, however fast the frames: each frame
# carries a value no frame of the last 30 had, so every step sees a change.

STIMULUS_FIRST_GOOD = 3  # a mailbox is VALID after 3 good frames
STIMULUS_RATE_HZ = 100.0
CHARGER_FLAG_VALUES = 31  # the five flags, never all clear
CHARGER_FLAGS = ("HardwareFailure", "OverTemperature", "InputVoltageWrong", "StartingState",
                 "CommTimeout")


class EventStimulus:
    def __init__(self, rig: Rig):
        self.rig = rig
        name = "INV_FaultCodes" if rig.image == "vcu" else "CHG_Status"
        self.msg = rig.control_db.get_message_by_name(name)
        self.step_s = rig.status_period_s if rig.image == "bms" else rig.inv_period_s
        self.bus = rig.control.bus
        self.n = 0
        self.failed: str | None = None  # why sending stopped, if it did

    def frames_for(self, events: int) -> int:
        """Frames to send for at least `events` Events: one a step."""
        per_step = max(1, round(STIMULUS_RATE_HZ * self.step_s)) + 1  # one for jitter
        return STIMULUS_FIRST_GOOD + events * per_step

    def _frame(self, value: int):
        if self.rig.image == "vcu":
            data = self.msg.encode({"PostFaults": value, "RunFaults": 0})
        else:
            flags = value % CHARGER_FLAG_VALUES + 1
            signals = {"OutputVoltage": 0, "OutputCurrent": 0}
            signals.update({n: (flags >> i) & 1 for i, n in enumerate(CHARGER_FLAGS)})
            data = self.msg.encode(signals)
        return self.rig.can.Message(arbitration_id=self.msg.frame_id,
                                    is_extended_id=self.msg.is_extended_frame, data=data)

    def send(self, events: int) -> int:
        """Sends enough frames for at least `events` Events, at
        STIMULUS_RATE_HZ on a deadline (not a sleep each, which Windows
        stretches). Returns the frames sent; stops early, with `failed`
        set, if the bus refuses one (the board lost power, say)."""
        period = 1.0 / STIMULUS_RATE_HZ
        due = time.perf_counter()
        total = self.frames_for(events)
        for i in range(total):
            self.n += 1
            try:
                self.bus.send(self._frame(self.n))
            except self.rig.can.CanError as e:
                self.failed = f"stopped after {i} frames: {e}"
                return i
            due += period
            while time.perf_counter() < due:
                time.sleep(max(0.0, min(0.002, due - time.perf_counter())))
        return total


def new_events(before: bytes, after: bytes) -> list[bl.Event]:
    """The Events in `after` that `before` did not hold: those of a newer
    boot, or of the same boot with a higher sequence number."""
    old = bl.parse_event_log(before)
    last = max(((e.boot, e.seq) for e in old), default=(-1, -1))
    return [e for e in bl.parse_event_log(after) if (e.boot, e.seq) > last]


# ---------------------------------------------------------------------------
# The reset cases (COM-06 to COM-10): one helper, five stimuli.

def reset_case(rig: Rig, stimulus, held: bool, not_power_on: bool, budget: str) -> int:
    """Runs a reset `args.runs` times. Each run: the stimulus, then the end
    state SETTLE_S after the first status frame. The BMS must end in
    FAULT_HELD if `held`, and not otherwise; the VCU always in GLV_ON.
    Both must report LastResetNotPowerOn as `not_power_on`. The time
    recorded is the last status frame before the reset to the first after,
    for the record only."""
    runs = []
    if rig.image == "bms" and held:
        rig.operator.do(DEBUG_BIT_PRECONDITION)
    for n in range(rig.args.runs):
        rig.wait_running()
        mark = rig.control.now()
        stimulus(rig)
        gap = rig.wait_reset(mark, timeout_s=15.0)
        if gap is None:
            runs.append(bl.Run(None, False, "no reset seen on the bus"))
            continue
        before, after = gap
        end = rig.settled_status(after)
        if end is None:
            runs.append(bl.Run(None, False, "no status frame after the reset"))
            continue
        state = rig.state_name(end)
        reported = rig.decode(end)["LastResetNotPowerOn"] == 1
        if rig.image == "bms":
            ok = (state == "FAULT_HELD") == held
        else:
            ok = state == "GLV_ON"
        ok = ok and reported == not_power_on
        runs.append(bl.Run(round((after - before) * 1000.0, 1), ok,
                           f"ended {state}, LastResetNotPowerOn {int(reported)}"))
        print(f"run {n + 1}: {runs[-1].time_ms} ms to the first status frame, {runs[-1].note}")
    if rig.image == "bms":
        never_high = not rig.operator.yes(
            "Scope on MCU_BMS_OK (PB0), normal trigger on a rising edge: did it trigger "
            "during these resets?")
        rig.notes.append(f"MCU_BMS_OK never high during the resets: {never_high}")
        if not never_high:
            rig.case_failures.append("MCU_BMS_OK went high during a reset")
    return rig.finish(budget, None, runs)
