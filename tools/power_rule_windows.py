#!/usr/bin/env python3
"""The one check of the 80 kW rule's two windows (issue 60, ADR 0017).

EV.3.3.1 limits the power leaving the pack to 80 kW. EV.3.4.1 judges it on
the Energy Meter over two windows; a Violation is either
  - "continuous": above the limit continuously for 100 ms, or
  - "average":    the 500 ms moving average above the limit.

The Power limiter's host closed-loop test, bench scripts playing BMS_Power
and car logs are all judged by violations() below, so a pass means the same
thing everywhere.

A trace is (time in ms, pack power in W) samples in time order. Each sample
holds until the next, as a meter that reports every 20 ms does; the last
one ends the trace. "Above" is strictly above: exactly 80 kW is allowed.
A row with an empty power cell is no reading: it ends the stretch before
it, nothing is held across it, and the time to the next reading is
reported as not judged. Each stretch is judged on its own. Both windows
are judged on the held power, the moving average over every 500 ms
window inside the trace (none before 500 ms of data).

    python tools/power_rule_windows.py log.csv [--time-column time_ms]
        [--power-column pack_power_w] [--limit 80000]

prints each Violation and exits 1 if there is one, 0 if there is none, and
2 if the file holds no power reading at all (nothing was judged: a log
from a car without its Energy Meter is not a pass).
Python 3.10 or later, standard library only.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import sys
from dataclasses import dataclass

LIMIT_W = 80_000          # EV.3.3.1
CONTINUOUS_MS = 100       # EV.3.4.1: continuously above for 100 ms
AVERAGE_MS = 500          # EV.3.4.1: 500 ms moving average


@dataclass(frozen=True)
class Violation:
    kind: str        # "continuous" or "average"
    start_ms: float  # the stretch above the limit, or the first window's end
    end_ms: float    # its end, or the last window's end
    peak_w: float    # the highest power, or the highest average, in it

    def __str__(self) -> str:
        what = "power" if self.kind == "continuous" else "500 ms average"
        return (f"{self.kind}: {what} above the limit from {self.start_ms} ms to "
                f"{self.end_ms} ms, peak {self.peak_w:.0f} W")


def _continuous(samples, limit_w):
    found = []
    i = 0
    n = len(samples)
    while i < n:
        if samples[i][1] <= limit_w:
            i += 1
            continue
        start = samples[i][0]
        peak = samples[i][1]
        j = i
        while j < n and samples[j][1] > limit_w:
            peak = max(peak, samples[j][1])
            j += 1
        # Held until the next sample, or to the last one at the end.
        end = samples[j][0] if j < n else samples[-1][0]
        if end - start >= CONTINUOUS_MS:
            found.append(Violation("continuous", start, end, peak))
        i = j
    return found


def _average(samples, limit_w):
    times = [t for t, _ in samples]
    # energy[i]: W.ms from the first sample to sample i, power held.
    energy = [0]
    for (t0, p0), (t1, _) in zip(samples, samples[1:]):
        energy.append(energy[-1] + p0 * (t1 - t0))

    def energy_at(x):
        i = bisect.bisect_right(times, x) - 1
        return energy[i] + samples[i][1] * (x - times[i])

    first, last = times[0], times[-1]
    # The held power is a step function, so the average is extreme with a
    # window edge on a sample: try every window that ends on one or starts
    # on one.
    ends = sorted({t for t in times if t - first >= AVERAGE_MS} |
                  {t + AVERAGE_MS for t in times if t + AVERAGE_MS <= last})
    found = []
    stretch = None
    for end in ends:
        e = energy_at(end) - energy_at(end - AVERAGE_MS)
        if e > limit_w * AVERAGE_MS:
            avg = e / AVERAGE_MS
            if stretch is None:
                stretch = [end, end, avg]
            else:
                stretch[1] = end
                stretch[2] = max(stretch[2], avg)
        elif stretch is not None:
            found.append(Violation("average", *stretch))
            stretch = None
    if stretch is not None:
        found.append(Violation("average", *stretch))
    return found


def violations(samples, limit_w=LIMIT_W):
    """Every Violation in a trace of (time ms, power W) samples, in time
    order, each window's stretches in order: continuous first, then
    average. An empty list is a pass."""
    samples = list(samples)
    if len(samples) < 2:
        return []
    return _continuous(samples, limit_w) + _average(samples, limit_w)


def read_csv(path, time_column="time_ms", power_column="pack_power_w"):
    """The trace in a CSV file, every row a sample."""
    with open(path, newline="") as f:
        return [(float(row[time_column]), float(row[power_column]))
                for row in csv.DictReader(f)]


def read_stretches(path, time_column="time_ms", power_column="pack_power_w"):
    """The traces in a CSV file whose power cell may be empty, and the time
    with no reading, in ms. A decoded step log leaves it empty while the
    meter's mailbox is not VALID (issue 61): nothing is known there, so an
    empty cell ends the stretch before it, the last reading held until that
    row, and no reading is carried across it."""
    stretches, current = [], []
    unknown = 0.0
    gap_from = None
    t = 0.0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            t = float(row[time_column])
            cell = row[power_column].strip()
            if not cell:
                if current:
                    current.append((t, current[-1][1]))
                    stretches.append(current)
                    current = []
                if gap_from is None:
                    gap_from = t
                continue
            if gap_from is not None:
                unknown += t - gap_from
                gap_from = None
            current.append((t, float(cell)))
    if current:
        stretches.append(current)
    if gap_from is not None:
        unknown += t - gap_from  # a gap that runs to the end of the file
    return stretches, unknown


def violations_in_csv(path, time_column="time_ms", power_column="pack_power_w",
                      limit_w=LIMIT_W):
    """Every Violation in a CSV file, each stretch between empty cells
    judged on its own (read_stretches())."""
    stretches, _ = read_stretches(path, time_column, power_column)
    return [v for s in stretches for v in violations(s, limit_w)]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("csv", help="a trace: one row per sample, with a header")
    ap.add_argument("--time-column", default="time_ms")
    ap.add_argument("--power-column", default="pack_power_w")
    ap.add_argument("--limit", type=float, default=LIMIT_W, help="watts")
    args = ap.parse_args(argv)

    stretches, unknown = read_stretches(args.csv, args.time_column, args.power_column)
    if not stretches:
        print(f"{args.csv}: no power reading at all: nothing judged")
        return 2
    found = [v for st in stretches for v in violations(st, args.limit)]
    for v in found:
        print(f"{args.csv}: {v}")
    if unknown > 0:
        print(f"{args.csv}: {unknown:g} ms with no power reading, not judged")
    if not found:
        print(f"{args.csv}: no Violation (EV.3.4.1)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
