#!/usr/bin/env python3
"""Turns one Data log file (ADR 0021) into CSV, one file per Record type.

    python tools/data_log_decode.py B00042/L0003.TDL [-o OUTDIR] [--summary FILE]

Writes <stem>.step.csv and <stem>.event.csv (each only if the file holds that
type) into OUTDIR (default: beside the input) and prints a summary: the file
header, each type's Record count and the Records lost in gaps of its sequence
numbers, Records skipped for a bad CRC, and the bytes of a torn last Record.

The format is App/common/logic/Inc/data_log_record.h; a change there changes
this file in the same commit. tests/host checks the two agree. Python 3.10 or
later, standard library only.
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"TDLG"
LAYOUT = 1
HEADER_SIZE = 32
SYNC = 0xA5
OVERHEAD = 14  # 12 bytes before the payload, the CRC-16 after

STEP = 1
EVENT = 2
TYPE_NAMES = {STEP: "step", EVENT: "event"}
IMAGE_NAMES = {1: "VCU", 2: "BMS"}


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
class Header:
    layout: int
    image: int
    dirty: bool
    build_id: int
    boot: int
    file: int
    opened_ms: int


@dataclass
class Record:
    type: int
    uptime_ms: int
    seq: int
    payload: bytes


@dataclass
class Decoded:
    header: Header
    records: list[Record] = field(default_factory=list)
    bad: int = 0
    torn: int = 0


def parse_header(data: bytes) -> Header:
    if len(data) < HEADER_SIZE:
        raise ValueError("shorter than a file header")
    if data[0:4] != MAGIC:
        raise ValueError("not a Data log file (no TDLG magic)")
    (crc,) = struct.unpack_from("<H", data, 30)
    if crc != crc16(data[0:30]):
        raise ValueError("file header CRC is wrong")
    layout, image, flags, build_id, boot, number, opened = struct.unpack_from(
        "<HBBIHHI", data, 4)
    if layout != LAYOUT:
        raise ValueError(f"layout {layout}; this decoder reads layout {LAYOUT}")
    return Header(layout, image, bool(flags & 1), build_id, boot, number, opened)


def record_at(data: bytes, at: int) -> Record | None | int:
    """The Record at offset at; None if no whole good one starts there; the
    count of bytes left if a Record starts there but runs past the end."""
    if data[at] != SYNC:
        return None
    if len(data) - at < 4:
        return len(data) - at
    (length,) = struct.unpack_from("<H", data, at + 2)
    end = at + 12 + length
    if end + 2 > len(data):
        return len(data) - at
    (crc,) = struct.unpack_from("<H", data, end)
    if crc != crc16(data[at:end]):
        return None
    uptime, seq = struct.unpack_from("<II", data, at + 4)
    return Record(data[at + 1], uptime, seq, data[at + 12:end])


def decode(data: bytes) -> Decoded:
    out = Decoded(parse_header(data))
    at = HEADER_SIZE
    while at < len(data):
        found = record_at(data, at)
        if isinstance(found, Record):
            out.records.append(found)
            at += OVERHEAD + len(found.payload)
            continue
        if isinstance(found, int) and not any(
                isinstance(record_at(data, i), Record) for i in range(at + 1, len(data))):
            # Runs past the end with nothing whole after it: cut off.
            out.torn = found
            break
        # Bad: skip to the next sync byte that starts a whole Record.
        out.bad += 1
        at += 1
        while at < len(data) and not isinstance(record_at(data, at), Record):
            at += 1
        if at >= len(data):
            break
    return out


def step_row(r: Record) -> list:
    count, state, flags, step_us, overruns = struct.unpack("<IBBHI", r.payload)
    return [r.seq, r.uptime_ms, count, state, flags & 1, (flags >> 1) & 1,
            (flags >> 2) & 1, (flags >> 3) & 1, step_us, overruns]


def event_row(r: Record) -> list:
    boot, code, uptime, seq = struct.unpack_from("<HHII", r.payload, 0)
    data = r.payload[12:28]
    (crc,) = struct.unpack_from("<H", r.payload, 30)
    return [r.seq, r.uptime_ms, boot, seq, uptime, code, data.hex(),
            int(crc == crc16(r.payload[0:30]))]


COLUMNS = {
    STEP: (["seq", "uptime_ms", "step_count", "state", "fault_active",
            "background_stalled", "timing_fault", "startup_ok", "step_us",
            "overruns"], step_row, 12),
    EVENT: (["seq", "uptime_ms", "boot", "event_seq", "event_uptime_ms", "code",
             "data", "crc_ok"], event_row, 32),
}


def gaps(seqs: list[int]) -> tuple[int, int]:
    """Records lost and gaps, from one type's sequence numbers in order."""
    lost = count = 0
    for prev, cur in zip(seqs, seqs[1:]):
        missing = (cur - prev - 1) & 0xFFFFFFFF
        if missing:
            lost += missing
            count += 1
    return lost, count


def summary(d: Decoded) -> str:
    h = d.header
    lines = [
        f"file: image {IMAGE_NAMES.get(h.image, h.image)}, build {h.build_id:08x}"
        f"{' dirty' if h.dirty else ''}, boot {h.boot}, file {h.file}, "
        f"layout {h.layout}, opened at {h.opened_ms} ms"
    ]
    for t in sorted({r.type for r in d.records}):
        seqs = [r.seq for r in d.records if r.type == t]
        lost, count = gaps(seqs)
        lines.append(f"{TYPE_NAMES.get(t, f'type {t}')}: {len(seqs)} records, "
                     f"{lost} lost in {count} gaps")
    lines.append(f"bad records: {d.bad}")
    lines.append(f"torn tail: {d.torn} bytes")
    return "\n".join(lines) + "\n"


def write_csvs(d: Decoded, outdir: Path, stem: str) -> None:
    outdir.mkdir(parents=True, exist_ok=True)
    for t, (columns, row, size) in COLUMNS.items():
        records = [r for r in d.records if r.type == t]
        if not records:
            continue
        with open(outdir / f"{stem}.{TYPE_NAMES[t]}.csv", "w", newline="") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(columns)
            for r in records:
                if len(r.payload) == size:
                    w.writerow(row(r))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("file", type=Path)
    p.add_argument("-o", "--outdir", type=Path)
    p.add_argument("--summary", type=Path, help="also write the summary here")
    args = p.parse_args()

    try:
        d = decode(args.file.read_bytes())
    except ValueError as e:
        print(f"{args.file}: {e}", file=sys.stderr)
        return 1

    write_csvs(d, args.outdir or args.file.parent, args.file.stem)
    text = summary(d)
    sys.stdout.write(text)
    if args.summary:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(text, newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
