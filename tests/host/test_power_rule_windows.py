"""Tests for tools/power_rule_windows.py, the one check of the 80 kW rule's
two windows (issue 60, ADR 0017) that the host closed-loop test, bench
scripts and car logs all use.

EV.3.3.1: pack power at most 80 kW. EV.3.4.1: a Violation is power above
the limit continuously for 100 ms, or a 500 ms moving average above it.

Run with:  python tests/host/test_power_rule_windows.py
(CTest runs it as test_power_rule_windows.)
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

import power_rule_windows as prw  # noqa: E402


def trace(*segments, step_ms=1):
    """[(duration_ms, watts), ...] as samples every step_ms, from t = 0."""
    samples = []
    t = 0
    for duration, watts in segments:
        for _ in range(0, duration, step_ms):
            samples.append((t, watts))
            t += step_ms
    return samples


def kinds(violations):
    return sorted({v.kind for v in violations})


class ContinuousWindow(unittest.TestCase):
    def test_a_steady_79_9_kw_is_no_violation(self):
        self.assertEqual([], prw.violations(trace((3000, 79_900))))

    def test_exactly_80_kw_is_not_above_the_limit(self):
        self.assertEqual([], prw.violations(trace((3000, 80_000))))

    def test_99_ms_above_is_no_violation(self):
        v = prw.violations(trace((1000, 50_000), (99, 90_000), (1000, 50_000)))
        self.assertEqual([], v)

    def test_100_ms_above_is_a_violation(self):
        v = prw.violations(trace((1000, 50_000), (100, 90_000), (1000, 50_000)))
        self.assertEqual(["continuous"], kinds(v))
        self.assertEqual(1000, v[0].start_ms)
        self.assertEqual(1100, v[0].end_ms)

    def test_a_sample_holds_until_the_next_one(self):
        # 20 ms samples, as the Energy Meter and BMS_Power send: five above
        # in a row hold the power above for 100 ms.
        samples = [(0, 50_000), (20, 90_000), (40, 90_000), (60, 90_000),
                   (80, 90_000), (100, 90_000), (120, 50_000), (140, 50_000)]
        self.assertEqual(["continuous"], kinds(prw.violations(samples)))
        samples[5] = (100, 50_000)
        self.assertEqual([], prw.violations(samples))


class AverageWindow(unittest.TestCase):
    def test_short_spikes_that_average_above_are_a_violation(self):
        # 90 ms at 90 kW, 10 ms at 60 kW: never 100 ms above, 87 kW average.
        v = prw.violations(trace(*[(90, 90_000), (10, 60_000)] * 10))
        self.assertEqual(["average"], kinds(v))

    def test_an_average_of_exactly_80_kw_is_no_violation(self):
        v = prw.violations(trace(*[(50, 90_000), (50, 70_000)] * 20))
        self.assertEqual([], v)

    def test_no_average_before_500_ms_of_data(self):
        # 400 ms at 85 kW alone: continuous, but no 500 ms window exists.
        v = prw.violations(trace((400, 85_000)))
        self.assertEqual(["continuous"], kinds(v))


class CommandLine(unittest.TestCase):
    def write_csv(self, rows):
        f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, newline="")
        f.write("time_ms,pack_power_w\n")
        for t, w in rows:
            f.write(f"{t},{w}\n")
        f.close()
        self.addCleanup(os.remove, f.name)
        return f.name

    def test_a_clean_file_exits_0(self):
        self.assertEqual(0, prw.main([self.write_csv(trace((1000, 70_000), step_ms=10))]))

    def test_a_violation_exits_1(self):
        self.assertEqual(1, prw.main([self.write_csv(trace((1000, 85_000), step_ms=10))]))

    def test_a_row_with_no_power_ends_the_reading_before_it(self):
        # A decoded step log leaves the power empty while the meter's mailbox
        # is not VALID (issue 61). Nothing is known there, so no reading is
        # held across it: 85 kW from 0 to 50 ms and from 100 to 150 ms is
        # two stretches of 50 ms, not 150 ms above the limit.
        path = self.write_csv([(0, 85_000), (50, ""), (100, 85_000), (150, 70_000),
                               (200, 70_000)])
        self.assertEqual([], prw.violations_in_csv(path))

    def test_a_long_stretch_between_gaps_is_still_judged(self):
        path = self.write_csv([(0, ""), (10, 85_000), (60, 85_000), (120, 70_000),
                               (130, "")])
        self.assertEqual([prw.Violation("continuous", 10.0, 120.0, 85_000.0)],
                         prw.violations_in_csv(path))

    def test_a_file_with_no_power_reading_at_all_is_not_a_pass(self):
        # A step log from a car without the Energy Meter (issue 61): nothing
        # was judged, so the check must not say "no Violation".
        path = self.write_csv([(0, ""), (10, ""), (20, "")])
        self.assertEqual(2, prw.main([path]))

    def test_a_gap_at_the_end_counts_as_not_judged(self):
        path = self.write_csv([(0, 70_000), (10, 70_000), (20, ""), (530, "")])
        _, unknown = prw.read_stretches(path)
        self.assertEqual(510.0, unknown)

    def test_a_file_with_gaps_says_how_much_was_not_judged(self):
        path = self.write_csv([(0, 70_000), (10, ""), (20, ""), (30, 70_000), (40, 70_000)])
        self.assertEqual(0, prw.main([path]))


if __name__ == "__main__":
    unittest.main()
