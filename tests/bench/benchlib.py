"""The pure helpers every Rig A bench script shares (issue 70,
docs/spec/bench-test-plan.md): judging a run, writing the result file,
judging Control bus frames, and reading bank 2 from an OpenOCD dump.

Nothing here touches a CAN adapter, a serial port or a debugger; that is
rig.py. Standard library only, Python 3.10 or later. Host-tested by
tests/host/test_benchlib.py against a bank 2 dump the image's own storage
code writes.

The layouts read here are App/common/logic/Inc/params.h and event_log.h;
a change there changes this file in the same commit.
"""
from __future__ import annotations

import binascii
import json
import re
import struct
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Bank 2 (ADR 0014). A dump starts at BANK2_DUMP_ADDR.

BANK2_DUMP_ADDR = 0x08040000
BANK2_DUMP_SIZE = 0x12000  # both Parameter copies and the Event log

PARAMS_COPY_OFFSETS = (0x0000, 0x1000)  # copy A, copy B (params.h)
PARAMS_COPY_SIZE = 0x1000
PARAMS_HEADER_SIZE = 24
PARAMS_MARKER = 0x4D524150  # "PARM"
PARAMS_IMAGE = {"vcu": 1, "bms": 2}

EVENT_LOG_OFFSET = 0x2000  # 0x08042000 (event_log.h)
EVENT_LOG_PAGES = 32
EVENT_LOG_PAGE_SIZE = 0x800
EVENT_SIZE = 32
EVENT_LOG_MARKER = 0x4C545645  # "EVTL"

# Event codes the scripts look for (event_codes.h).
EVENT_STORAGE_REPAIRED = 4
EVENT_EVENTS_DROPPED = 5
EVENT_PARAM_SAVE = 18
EVENT_OVERRUN = 32

# The flood guard (event_flood.h).
EVENT_FLOOD_BURST = 64
EVENT_FLOOD_REFILL_MS = 1000


def crc16(data: bytes) -> int:
    """CRC-16/IBM-3740: poly 0x1021, init 0xFFFF, not reflected (crc16.h)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


@dataclass
class Event:
    page: int  # its place in bank 2, 0 to 31
    page_number: int
    boot: int
    code: int
    uptime_ms: int
    seq: int
    data: bytes
    crc_ok: bool


def _log_pages(bank2: bytes) -> list[tuple[int, int]]:
    """(page number, place) of every page with a good header, oldest first."""
    pages = []
    for place in range(EVENT_LOG_PAGES):
        start = EVENT_LOG_OFFSET + place * EVENT_LOG_PAGE_SIZE
        header = bank2[start:start + EVENT_SIZE]
        if len(header) < EVENT_SIZE:
            break
        marker, number = struct.unpack_from("<II", header)
        (crc,) = struct.unpack_from("<H", header, 30)
        if marker == EVENT_LOG_MARKER and crc == crc16(header[:30]):
            pages.append((number, place))
    return sorted(pages)


def event_log_page_numbers(bank2: bytes) -> list[int]:
    """The page numbers of the pages in use, oldest first. Each page opened
    (and so each page erased) adds one."""
    return [number for number, _ in _log_pages(bank2)]


def parse_event_log(bank2: bytes) -> list[Event]:
    """Every Event written, in ring order. A slot that is still erased ends
    its page; a slot whose CRC is wrong is returned with crc_ok False."""
    events = []
    for number, place in _log_pages(bank2):
        start = EVENT_LOG_OFFSET + place * EVENT_LOG_PAGE_SIZE
        for slot in range(1, EVENT_LOG_PAGE_SIZE // EVENT_SIZE):
            raw = bank2[start + slot * EVENT_SIZE:start + (slot + 1) * EVENT_SIZE]
            if raw == b"\xff" * EVENT_SIZE:
                break
            boot, code, uptime, seq = struct.unpack_from("<HHII", raw)
            (crc,) = struct.unpack_from("<H", raw, 30)
            events.append(Event(place, number, boot, code, uptime, seq, bytes(raw[12:28]),
                                crc == crc16(raw[:30])))
    return events


@dataclass
class ParamCopy:
    status: str  # "good", "empty", "bad header" or "bad CRC"
    image: int = 0
    save_counter: int = 0
    values: dict[int, int] = field(default_factory=dict)  # ID: value


def parse_param_copy(copy: bytes) -> ParamCopy:
    """One Parameter copy as params_load() judges it, short of the image."""
    marker, image, counter, length, _reserved, crc = struct.unpack_from("<IIIIII", copy)
    if marker == 0xFFFFFFFF:
        return ParamCopy("empty")
    if marker != PARAMS_MARKER or length % 8 or length > PARAMS_COPY_SIZE - PARAMS_HEADER_SIZE:
        return ParamCopy("bad header")
    pairs = copy[PARAMS_HEADER_SIZE:PARAMS_HEADER_SIZE + length]
    if binascii.crc32(copy[:20] + pairs) != crc:
        return ParamCopy("bad CRC", image, counter)
    values = {}
    for i in range(0, length, 8):
        pid, _zero, value = struct.unpack_from("<HHi", pairs, i)
        values[pid] = value
    return ParamCopy("good", image, counter, values)


def parse_param_copies(bank2: bytes) -> tuple[ParamCopy, ParamCopy]:
    """Copy A and copy B."""
    a, b = (parse_param_copy(bank2[o:o + PARAMS_COPY_SIZE]) for o in PARAMS_COPY_OFFSETS)
    return a, b


def params_in_force(bank2: bytes, image: str) -> dict[int, int] | None:
    """The stored values the image loads at start-up: the newest good copy
    of its own image. None: no such copy, so every parameter is defaulted."""
    good = [c for c in parse_param_copies(bank2)
            if c.status == "good" and c.image == PARAMS_IMAGE[image]]
    if not good:
        return None
    return max(good, key=lambda c: c.save_counter).values


def corrupt_param_copy(bank2: bytes, which: int) -> bytes:
    """bank2 with one bit flipped in copy `which` (0 A, 1 B), inside what
    its CRC covers: the first stored value, or the CRC itself if the copy
    holds no pairs. The copy then reads "bad CRC"."""
    blob = bytearray(bank2)
    start = PARAMS_COPY_OFFSETS[which]
    (length,) = struct.unpack_from("<I", blob, start + 12)
    target = start + PARAMS_HEADER_SIZE + 4 if length >= 8 else start + 20
    blob[target] ^= 0x01
    return bytes(blob)


# ---------------------------------------------------------------------------
# Control bus frames (ADR 0004, can/control.dbc).

@dataclass
class Frame:
    t: float  # seconds, the adapter's timestamp
    can_id: int
    data: bytes


STATUS_ID = {"bms": 0x080, "vcu": 0x090}
INV_COMMAND_ID = 0x0C0

# The frames each image sends on every step or every second step, with
# their period in ms: these are judged late. The rest (BMS_Limits and the
# other 100 ms frames) are sent in one slot of five and only counted.
EVERY_STEP_MS = {
    "bms": {0x080: 20.0, 0x082: 20.0},
    "vcu": {0x0C0: 10.0, 0x090: 20.0},
}

# Late: a gap between two frames of one ID over 1.5 periods (chosen in
# issue 70; the plan states it).
LATE_FACTOR = 1.5

# A reset shows as a gap of at least this many periods.
RESET_GAP_PERIODS = 3

E2E_IDS = (0x080, 0x081, 0x082, 0x090)  # Profile 11 counter 0 to 14
E2E_COUNTER_MODULO = 15
INV_COUNTER_MODULO = 16


def frame_counter(frame: Frame) -> int | None:
    """The frame's own counter, or None for a frame without one."""
    if frame.can_id in E2E_IDS:
        return frame.data[1] & 0x0F
    if frame.can_id == INV_COMMAND_ID:
        return frame.data[5] >> 4
    return None


@dataclass
class BusJudgement:
    worst_gap_ms: dict[int, float]  # per judged ID
    late: list[tuple[int, float, float]]  # (ID, time of the late frame, gap ms)
    skipped: list[tuple[int, float]]  # (ID, time of the frame after the skip)

    @property
    def clean(self) -> bool:
        return not self.late and not self.skipped


def judge_control_bus(frames: list[Frame], image: str) -> BusJudgement:
    """Judges one recording of the Control bus: every frame with a counter
    must step it by one, and every every-step frame must come within
    LATE_FACTOR periods of the one before."""
    periods = EVERY_STEP_MS[image]
    last: dict[int, Frame] = {}
    worst: dict[int, float] = {}
    late = []
    skipped = []
    for f in sorted(frames, key=lambda f: f.t):
        prev = last.get(f.can_id)
        last[f.can_id] = f
        if prev is None:
            continue
        counter = frame_counter(f)
        if counter is not None:
            modulo = INV_COUNTER_MODULO if f.can_id == INV_COMMAND_ID else E2E_COUNTER_MODULO
            if counter != (frame_counter(prev) + 1) % modulo:
                skipped.append((f.can_id, f.t))
        if f.can_id in periods:
            gap_ms = round((f.t - prev.t) * 1000.0, 3)
            worst[f.can_id] = max(worst.get(f.can_id, 0.0), gap_ms)
            if gap_ms > periods[f.can_id] * LATE_FACTOR:
                late.append((f.can_id, f.t, gap_ms))
    return BusJudgement(worst, late, skipped)


def reset_gap(frames: list[Frame], can_id: int, after_s: float,
              period_s: float) -> tuple[float, float] | None:
    """(last frame before, first frame after) the first gap of at least
    RESET_GAP_PERIODS periods in can_id's frames that ends after after_s,
    or None if there is none."""
    times = sorted(f.t for f in frames if f.can_id == can_id)
    for before, after in zip(times, times[1:]):
        if after > after_s and after - before >= RESET_GAP_PERIODS * period_s:
            return before, after
    return None


# ---------------------------------------------------------------------------
# The Event log's flood guard (event_flood.h).

def _admitted(events: list[Event]) -> list[Event]:
    """The Events that took a token: all but the guard's own reports."""
    return [e for e in events if e.code != EVENT_EVENTS_DROPPED]


def flood_excess(events: list[Event]) -> list[tuple[int, int, int]]:
    """Every stretch of one boot's Events that holds more than the guard
    lets through: a burst of EVENT_FLOOD_BURST plus one refill per
    EVENT_FLOOD_REFILL_MS begun. Each as (first uptime, last uptime, count).
    Empty when the guard held."""
    admitted = _admitted(events)
    excess = []
    for i, first in enumerate(admitted):
        for j in range(i + EVENT_FLOOD_BURST, len(admitted)):
            span = admitted[j].uptime_ms - first.uptime_ms
            count = j - i + 1
            if count > EVENT_FLOOD_BURST + span // EVENT_FLOOD_REFILL_MS + 1:
                excess.append((first.uptime_ms, admitted[j].uptime_ms, count))
    return excess


def spacing_after_burst(events: list[Event]) -> int | None:
    """The closest two admitted Events after the first EVENT_FLOOD_BURST,
    in ms: about EVENT_FLOOD_REFILL_MS under a steady flood. None if there
    are not two."""
    tail = _admitted(events)[EVENT_FLOOD_BURST - 1:]
    gaps = [b.uptime_ms - a.uptime_ms for a, b in zip(tail, tail[1:])]
    return min(gaps) if gaps else None


# ---------------------------------------------------------------------------
# Shell output (App/common/Src/shell.c, control_bus.c, reset_info.c).

def _first_int(pattern: str, text: str) -> int | None:
    m = re.search(pattern, text)
    return int(m.group(1)) if m else None


def parse_overruns(step_text: str) -> int | None:
    """The Overrun count from `step`."""
    return _first_int(r"\boverruns (\d+)", step_text)


def parse_tx_skipped(bus_text: str) -> int | None:
    """Control bus frames skipped for no free send slot, from `bus`."""
    return _first_int(r"\btx_skipped=(\d+)", bus_text)


def parse_last_reset(text: str) -> str | None:
    """The last reset's cause from `lastreset`, e.g. "watchdog"."""
    m = re.search(r"last reset:\s+(.+?) \(RCC_CSR", text)
    return m.group(1) if m else None


def parse_crash_record(text: str) -> str | None:
    """The Crash record from `lastreset` ("<cause> in <task>"), or None."""
    m = re.search(r"crash record:\s+(.+?)(?:, (?:at|address)|\r|\n|$)", text)
    if not m or m.group(1) == "none":
        return None
    return m.group(1)


# ---------------------------------------------------------------------------
# Runs, the verdict and the result file (docs/spec/bench-test-plan.md,
# "Results").

RUNS = 20


@dataclass
class Run:
    time_ms: float | None  # None: the case times nothing in this run
    ok: bool  # every check of the run other than its time held
    note: str = ""


@dataclass
class Result:
    case: str
    title: str
    image: str
    image_commit: str
    board: str
    budget: str  # as the plan states it
    budget_ms: float | None  # None: pass or fail on the runs alone
    runs: list[Run]
    date: str = field(default_factory=lambda: datetime.now(timezone.utc)
                      .isoformat(timespec="seconds"))
    notes: list[str] = field(default_factory=list)
    scope_screenshot: str | None = None

    @property
    def times_ms(self) -> list[float | None]:
        return [r.time_ms for r in self.runs]

    @property
    def worst_ms(self) -> float | None:
        times = [t for t in self.times_ms if t is not None]
        return max(times) if times else None

    @property
    def verdict(self) -> str:
        """PASS: RUNS runs, each judged ok, the worst at or under the
        budget with no margin. INCOMPLETE: fewer runs. FAIL otherwise."""
        failed = not all(r.ok for r in self.runs) or (
            self.budget_ms is not None and self.worst_ms is not None
            and self.worst_ms > self.budget_ms)
        if failed:
            return "FAIL"
        if len(self.runs) < RUNS:
            return "INCOMPLETE"
        return "PASS"

    def to_json(self) -> dict:
        out = asdict(self)
        out["times_ms"] = self.times_ms
        out["worst_ms"] = self.worst_ms
        out["verdict"] = self.verdict
        return out


def write_result(result: Result, directory: str | Path) -> Path:
    """Writes one run of a case as <case>_<image>_<UTC time>.json."""
    stamp = result.date.replace(":", "").replace("-", "").replace("+0000", "Z")
    path = Path(directory) / f"{result.case}_{result.image}_{stamp}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(result.to_json(), indent=2) + "\n", encoding="utf-8")
    return path
