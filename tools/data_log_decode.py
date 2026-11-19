#!/usr/bin/env python3
"""Turns one Data log file (ADR 0021) into CSV, one file per Record type.

    python tools/data_log_decode.py B00042/L0003.TDL [-o OUTDIR] [--summary FILE]
        [--control-dbc can/control.dbc] [--daq-dbc can/daq.dbc]

Writes <stem>.<type>.csv for each Record type the file holds (step, event,
vcu_step, imu, daq_rx, daq_tx, bms_pack, bms_snapshot) into OUTDIR (default:
beside the input) and
prints a summary: the file header, each type's Record count and the Records
lost in gaps of its sequence numbers, Records skipped for a bad CRC, and the
bytes of a torn last Record.

The VCU's step Records name their mailboxes by the map the file starts with;
each mailbox's frame and the 0x0C0 sent are decoded with the Control bus DBC,
and DAQ bus frames with the DAQ bus DBC. A mailbox that is not VALID holds
its Substitute, and the power column derived from it (bms_power_w,
em_power_w) is left empty. Judge the 80 kW rule on a decoded step log with

    python tools/power_rule_windows.py <stem>.vcu_step.csv \\
        --time-column uptime_ms --power-column em_power_w

The BMS's Records: bms_pack.csv has every step's pack-level fields;
bms_snapshot.csv has each snapshot's cell voltages (v_MM_C, V; empty if not
read), sensor readings (t_MM_C_mv) and their temperatures by the firmware's
own table (t_MM_C, C; empty outside 1300-2440 mV) and bleed flags (b_MM_C),
module MM, cell C; its step_count matches that step's bms_pack row.

The format is App/common/logic/Inc/data_log_record.h and, for each image's
Records, App/vcu/logic/Inc/vcu_log.h and App/bms/logic/Inc/bms_log.h; a
change there changes this file in the same commit. tests/host checks they
agree. Python 3.10 or later. The DBC decoding needs cantools
(requirements.txt); a file with no VCU step or DAQ Records needs only the
standard library.
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
VCU_STEP = 3
IMU = 4
DAQ_RX = 5
DAQ_TX = 6
MAILBOXES = 7
BMS_PACK = 8
BMS_SNAPSHOT = 9
TYPE_NAMES = {STEP: "step", EVENT: "event", VCU_STEP: "vcu_step", IMU: "imu",
              DAQ_RX: "daq_rx", DAQ_TX: "daq_tx", MAILBOXES: "mailboxes",
              BMS_PACK: "bms_pack", BMS_SNAPSHOT: "bms_snapshot"}
IMAGE_NAMES = {1: "VCU", 2: "BMS"}

REPO = Path(__file__).resolve().parent.parent
CAN_DIR = REPO / "can"
CELL_TEMP_SCRIPT = REPO / "App" / "bms" / "logic" / "gen_cell_temp.py"

# imu.h: the full scales imu.c configures (+/-8 g, +/-2000 dps).
ACCEL_G_PER_LSB = 0.244 / 1000
GYRO_DPS_PER_LSB = 70.0 / 1000

# data_log_record.h: the bits on top of a CAN ID in a DAQ frame Record and the map.
ID_MASK = 0x1FFFFFFF
ID_EXTENDED = 0x80000000
DAQ_FD = 0x40000000
DAQ_BRS = 0x20000000

# vcu_log.h
MAILBOX_AT = 65
MAILBOX_STATES = ["NODATA", "VALID", "INVALID"]
SENSOR_STATUS = ["OK", "NO_SAMPLE", "SUPPLY_FAULT", "NOT_CALIBRATED", "OUT_OF_RANGE"]
INPUT_BITS = ["sdc_closed", "start_pressed", "can_running", "bus_off_latched",
              "error_passive", "rx_overflowed", "samples_ready", "brake_engaged"]
DECIDED_BITS = ["limiter_active", "lower_ceiling", "command_sent", "fault_clear_sent",
                "status_sent", "brake_light", "fault_led", "pedal_check_due"]
INV_COMMAND_ID = 0x0C0

# bms_log.h
BMS_PACK_SIZE = 52
BMS_CELLS = 128
BMS_SNAPSHOT_SIZE = BMS_PACK_SIZE + 4 * BMS_CELLS + 16
BMS_INPUT_BITS = ["swept", "sdc_closed", "nfault_ok", "bms_ok_fb", "can_running",
                  "bus_off_latched", "error_passive", "rx_overflowed"]
BMS_DECIDED_BITS = ["bms_ok", "under_low_voltage_limit", "open_wire_due", "charger_valid",
                    "current_valid", "bridge_read", "balance_command"]
# bq_chain.h bq_chain_state_t and bms_charge.h bms_charge_state_t, in order.
CHAIN_STATES = ["WAKE", "STACK_WAKE", "STACK_WAIT", "ADDRESS", "SYNC", "VERIFY",
                "CONFIGURE", "PROTECT", "CLEAR", "RUNNING", "RETRY_WAIT"]
CHARGE_STATES = ["OFF", "WAITING", "CHARGING", "COMPLETE"]


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


# ---- the Records every image writes ----------------------------------------

STEP_COLUMNS = ["seq", "uptime_ms", "step_count", "state", "fault_active",
                "background_stalled", "timing_fault", "startup_ok", "step_us", "overruns"]


def step_row(r: Record) -> list:
    count, state, flags, step_us, overruns = struct.unpack("<IBBHI", r.payload[:12])
    return [r.seq, r.uptime_ms, count, state, flags & 1, (flags >> 1) & 1,
            (flags >> 2) & 1, (flags >> 3) & 1, step_us, overruns]


def event_row(r: Record) -> list:
    boot, code, uptime, seq = struct.unpack_from("<HHII", r.payload, 0)
    data = r.payload[12:28]
    (crc,) = struct.unpack_from("<H", r.payload, 30)
    return [r.seq, r.uptime_ms, boot, seq, uptime, code, data.hex(),
            int(crc == crc16(r.payload[0:30]))]


def imu_row(r: Record) -> list:
    raw = struct.unpack("<6h", r.payload)
    return ([r.seq, r.uptime_ms, *raw] +
            [round(v * ACCEL_G_PER_LSB, 6) for v in raw[:3]] +
            [round(v * GYRO_DPS_PER_LSB, 6) for v in raw[3:]])


FIXED = {
    STEP: (STEP_COLUMNS, step_row, 12),
    EVENT: (["seq", "uptime_ms", "boot", "event_seq", "event_uptime_ms", "code",
             "data", "crc_ok"], event_row, 32),
    IMU: (["seq", "uptime_ms", "ax_raw", "ay_raw", "az_raw", "gx_raw", "gy_raw", "gz_raw",
           "ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps"], imu_row, 12),
}


# ---- the Message database --------------------------------------------------

_dbs: dict[Path, object] = {}


def load_dbc(path: Path):
    """The DBC at path, loaded once; cantools is imported only then."""
    if path not in _dbs:
        try:
            import cantools  # only VCU step and DAQ Records need it
        except ImportError:
            raise ValueError("decoding VCU step and DAQ Records needs cantools: "
                             "pip install -r requirements.txt") from None
        _dbs[path] = cantools.database.load_file(str(path))
    return _dbs[path]


def find_message(db, frame_id: int):
    try:
        return db.get_message_by_frame_id(frame_id)
    except KeyError:
        return None


def decode_signals(msg, data: bytes) -> dict | None:
    """Each signal's scaled value; None for a frame the DBC cannot read."""
    try:
        return msg.decode(data, decode_choices=False, allow_truncated=True)
    except Exception:  # cantools raises several kinds; the frame stays raw
        return None


def signal_cells(msg, signals: dict | None) -> list:
    if msg is None:
        return []
    return [str(signals[s.name]) if signals and s.name in signals else "" for s in msg.signals]


def signal_columns(prefix: str, msg) -> list[str]:
    return [f"{prefix}.{s.name}" for s in msg.signals] if msg else []


# ---- DAQ bus frames --------------------------------------------------------

DAQ_COLUMNS = ["seq", "uptime_ms", "id", "extended", "fd", "brs", "len", "data", "name",
               "signals"]


def daq_row(r: Record, db) -> list:
    raw_id, length = struct.unpack_from("<IB", r.payload, 0)
    data = r.payload[5:5 + length]
    frame_id = raw_id & ID_MASK
    msg = find_message(db, frame_id)
    signals = decode_signals(msg, data) if msg else None
    return [r.seq, r.uptime_ms, hex(frame_id), int(bool(raw_id & ID_EXTENDED)),
            int(bool(raw_id & DAQ_FD)), int(bool(raw_id & DAQ_BRS)), length, data.hex(),
            msg.name if msg else "",
            ";".join(f"{k}={v}" for k, v in signals.items()) if signals else ""]


# ---- the VCU's step Record (vcu_log.h) -------------------------------------

def sensor_status(b: int) -> str:
    return SENSOR_STATUS[b] if b < len(SENSOR_STATUS) else str(b)


def power(signals: dict | None, current: str, voltage: str):
    return signals[current] * signals[voltage] if signals else ""


class VcuStep:
    """The columns and rows of a file's VCU step Records, named by its map."""

    def __init__(self, map_payload: bytes | None, db, first: Record):
        self.mailboxes = []  # (column prefix, message or None), in Record order
        self.notes = []      # what the summary must say about these Records
        self.skipped = 0     # Records whose mailboxes do not match the map
        if map_payload:
            for i in range(map_payload[0]):
                (raw,) = struct.unpack_from("<I", map_payload, 1 + 4 * i)
                msg = find_message(db, raw & ID_MASK)
                self.mailboxes.append((msg.name if msg else f"0x{raw & ID_MASK:x}", msg))
        else:
            # The map was lost: name the mailboxes by position, leave them raw.
            count = first.payload[64] if len(first.payload) > MAILBOX_AT else 0
            self.mailboxes = [(f"mailbox{i}", None) for i in range(count)]
            self.notes.append("vcu_step: no mailbox map: mailboxes named by position, "
                              "not decoded")
        self.command = find_message(db, INV_COMMAND_ID)
        self.columns = (STEP_COLUMNS + INPUT_BITS + DECIDED_BITS +
                        ["apps1_counts", "apps2_counts", "brake1_counts", "brake2_counts",
                         "supply1_counts", "supply2_counts", "apps1_travel_pct",
                         "apps2_travel_pct", "apps1_status", "apps2_status",
                         "brake1_pressure_counts", "brake2_pressure_counts",
                         "brake1_status", "brake2_status", "start_blockers", "cuts",
                         "torque_request_nm", "torque_nm", "torque_ceiling_nm",
                         "power_ceiling_w", "trim_ppm", "command"] +
                        signal_columns("INV_Command", self.command) +
                        ["bms_power_w", "em_power_w"])
        for name, msg in self.mailboxes:
            self.columns += [f"{name}.state", f"{name}.data"] + signal_columns(name, msg)

    def row(self, r: Record) -> list | None:
        """None for a Record whose mailboxes do not match the map."""
        p = r.payload
        if len(p) <= MAILBOX_AT or len(p) != MAILBOX_AT + 9 * p[64] \
                or p[64] != len(self.mailboxes):
            self.skipped += 1
            return None
        inputs, decided = p[12], p[13]
        counts = struct.unpack_from("<6H", p, 14)
        travel = struct.unpack_from("<2H", p, 26)
        pressure = struct.unpack_from("<2H", p, 32)
        blockers, cuts, request, torque, ceiling = struct.unpack_from("<HHhhh", p, 38)
        power_ceiling, trim = struct.unpack_from("<Ii", p, 48)
        sent = bool(decided & 0x04)

        decided_cells = [(decided >> i) & 1 for i in range(8)]
        decided_cells[1] = "discharge" if decided & 0x02 else "target"
        out = (step_row(r) + [(inputs >> i) & 1 for i in range(8)] + decided_cells +
               list(counts) + [t / 10 for t in travel] +
               [sensor_status(p[30]), sensor_status(p[31])] +
               list(pressure) + [sensor_status(p[36]), sensor_status(p[37])] +
               [f"0x{blockers:04x}", f"0x{cuts:04x}", request / 10, torque / 10,
                ceiling / 10, power_ceiling, trim, p[56:64].hex() if sent else ""])
        if self.command:
            out += signal_cells(self.command,
                                decode_signals(self.command, p[56:64]) if sent else None)

        valid = {}
        boxes = []
        for i, (name, msg) in enumerate(self.mailboxes):
            at = MAILBOX_AT + 9 * i
            state = MAILBOX_STATES[p[at]] if p[at] < len(MAILBOX_STATES) else str(p[at])
            data = p[at + 1:at + 9]
            signals = decode_signals(msg, data[:msg.length]) if msg else None
            boxes += [state, data.hex()] + signal_cells(msg, signals)
            if state == "VALID":
                valid[name] = signals
        out += [power(valid.get("BMS_Power"), "PackCurrent", "PackVoltage"),
                power(valid.get("EM_Measurement"), "Current", "Voltage")]
        return out + boxes


# ---- the BMS's Records -----------------------------------------------------

_deci_celsius = None


def cell_temp_c(mv: int) -> float | None:
    """A sensor reading's temperature by the firmware's own table
    (App/bms/logic/gen_cell_temp.py); None outside its range."""
    global _deci_celsius
    if _deci_celsius is None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("gen_cell_temp", CELL_TEMP_SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _deci_celsius = (module.deci_celsius, module.MV_MIN, module.MV_MAX)
    convert, lo, hi = _deci_celsius
    return convert(mv) / 10 if lo <= mv <= hi else None


def cell_name(index: int) -> str:
    return f"{index // 8}.{index % 8}"


def name_of(names: list[str], value: int) -> str:
    return names[value] if value < len(names) else str(value)


BMS_PACK_COLUMNS = (
    STEP_COLUMNS + BMS_INPUT_BITS + BMS_DECIDED_BITS +
    ["faults_active", "faults_seen", "chain_state", "charge_state", "balance_state",
     "balance_reason", "devices_read", "bridge_fault", "cells_read", "sensors_read",
     "pack_voltage_v", "min_cell_v", "max_cell_v", "min_cell", "max_cell",
     "min_temp_c", "max_temp_c", "min_sensor", "max_sensor", "pack_current_a",
     "discharge_limit_a", "charge_limit_a", "cell_spread_mv"])


def bms_pack_row(r: Record) -> list:
    p = r.payload
    inputs, decided, active, seen, chain, charge, bal_state, bal_reason, devices, \
        bridge, cells, sensors = struct.unpack_from("<BBHHBBBBHBBB", p, 12)
    pack_mv, min_mv, max_mv, min_cell, max_cell, min_dc, max_dc, min_sensor, max_sensor, \
        current, discharge, charge_limit, spread = struct.unpack_from("<IHHBBhhBBhHHH", p, 28)
    return (step_row(r) +
            [(inputs >> i) & 1 for i in range(len(BMS_INPUT_BITS))] +
            [(decided >> i) & 1 for i in range(len(BMS_DECIDED_BITS))] +
            [f"0x{active:04x}", f"0x{seen:04x}", name_of(CHAIN_STATES, chain),
             name_of(CHARGE_STATES, charge), bal_state, bal_reason, f"0x{devices:04x}",
             bridge, cells, sensors, pack_mv / 1000, min_mv / 1000, max_mv / 1000,
             cell_name(min_cell), cell_name(max_cell), min_dc / 10, max_dc / 10,
             cell_name(min_sensor), cell_name(max_sensor), current / 10, discharge / 10,
             charge_limit / 10, spread])


def cell_columns(prefix: str, suffix: str = "") -> list[str]:
    return [f"{prefix}_{i // 8:02d}_{i % 8}{suffix}" for i in range(BMS_CELLS)]


BMS_SNAPSHOT_COLUMNS = (["seq", "uptime_ms", "step_count"] + cell_columns("v") +
                        cell_columns("t") + cell_columns("t", "_mv") + cell_columns("b"))


def bms_snapshot_row(r: Record) -> list:
    p = r.payload
    (count,) = struct.unpack_from("<I", p, 0)
    cells = struct.unpack_from(f"<{BMS_CELLS}H", p, BMS_PACK_SIZE)
    temps = struct.unpack_from(f"<{BMS_CELLS}H", p, BMS_PACK_SIZE + 2 * BMS_CELLS)
    bleed = p[BMS_PACK_SIZE + 4 * BMS_CELLS:]
    return ([r.seq, r.uptime_ms, count] +
            [mv / 1000 if mv else "" for mv in cells] +
            [("" if (c := cell_temp_c(mv)) is None else c) for mv in temps] +
            list(temps) +
            [(bleed[i // 8] >> (i % 8)) & 1 for i in range(BMS_CELLS)])


# ---- the file --------------------------------------------------------------

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


def tables(d: Decoded, control_dbc: Path, daq_dbc: Path,
           notes: list[str] | None = None) -> dict[int, tuple[list, list]]:
    """Each type's columns and rows. A Record of the wrong size is left out;
    notes gets what the summary must say about the VCU's step Records."""
    def of(t: int) -> list[Record]:
        return [r for r in d.records if r.type == t]

    notes = [] if notes is None else notes
    out = {}
    for t, (columns, row, size) in FIXED.items():
        if of(t):
            out[t] = (columns, [row(r) for r in of(t) if len(r.payload) == size])
    for t in (DAQ_RX, DAQ_TX):
        if of(t):
            db = load_dbc(daq_dbc)
            out[t] = (DAQ_COLUMNS, [daq_row(r, db) for r in of(t)
                                    if len(r.payload) >= 5 and
                                    len(r.payload) == 5 + r.payload[4]])
    if of(BMS_PACK):
        out[BMS_PACK] = (BMS_PACK_COLUMNS, [bms_pack_row(r) for r in of(BMS_PACK)
                                            if len(r.payload) == BMS_PACK_SIZE])
    if of(BMS_SNAPSHOT):
        out[BMS_SNAPSHOT] = (BMS_SNAPSHOT_COLUMNS,
                             [bms_snapshot_row(r) for r in of(BMS_SNAPSHOT)
                              if len(r.payload) == BMS_SNAPSHOT_SIZE])
    if of(VCU_STEP):
        maps = of(MAILBOXES)
        step = VcuStep(maps[0].payload if maps else None, load_dbc(control_dbc),
                       of(VCU_STEP)[0])
        out[VCU_STEP] = (step.columns,
                         [x for x in map(step.row, of(VCU_STEP)) if x is not None])
        notes += step.notes
        if step.skipped:
            notes.append(f"vcu_step: {step.skipped} records left out: their mailboxes "
                         "do not match the file's map")
    return out


def write_csvs(d: Decoded, outdir: Path, stem: str,
               control_dbc: Path = CAN_DIR / "control.dbc",
               daq_dbc: Path = CAN_DIR / "daq.dbc") -> list[str]:
    """Writes the CSVs; returns the notes for the summary."""
    notes: list[str] = []
    outdir.mkdir(parents=True, exist_ok=True)
    for t, (columns, rows) in sorted(tables(d, control_dbc, daq_dbc, notes).items()):
        with open(outdir / f"{stem}.{TYPE_NAMES[t]}.csv", "w", newline="") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(columns)
            w.writerows(rows)
    return notes


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("file", type=Path)
    p.add_argument("-o", "--outdir", type=Path)
    p.add_argument("--summary", type=Path, help="also write the summary here")
    p.add_argument("--control-dbc", type=Path, default=CAN_DIR / "control.dbc")
    p.add_argument("--daq-dbc", type=Path, default=CAN_DIR / "daq.dbc")
    args = p.parse_args()

    try:
        d = decode(args.file.read_bytes())
        notes = write_csvs(d, args.outdir or args.file.parent, args.file.stem,
                           args.control_dbc, args.daq_dbc)
    except ValueError as e:
        print(f"{args.file}: {e}", file=sys.stderr)
        return 1

    text = summary(d) + "".join(f"{n}\n" for n in notes)
    sys.stdout.write(text)
    if args.summary:
        args.summary.parent.mkdir(parents=True, exist_ok=True)
        args.summary.write_text(text, newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
