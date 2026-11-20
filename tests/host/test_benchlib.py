"""Tests for tests/bench/benchlib.py, the pure helpers every Rig A bench
script shares (issue 70, docs/spec/bench-test-plan.md).

The bank 2 parsers are checked against bench_bank2.bin, which
test_bench_bank2 writes with the image's own Parameter and Event log code
(tests/host/common/test_bench_bank2.c says what it holds), so the layout is
checked against the C, not against itself.

Run with:  python tests/host/test_benchlib.py <dir holding bench_bank2.bin>
(CTest runs it as test_benchlib.)
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "bench"))

import benchlib as bl  # noqa: E402

SAMPLE_DIR = None


def sample_bank2():
    with open(os.path.join(SAMPLE_DIR, "bench_bank2.bin"), "rb") as f:
        return f.read()


class EventLog(unittest.TestCase):
    def test_reads_every_event_in_ring_order(self):
        events = bl.parse_event_log(sample_bank2())

        self.assertEqual(73, len(events))
        self.assertEqual([(1, 1, 5), (1, 1, 6), (1, 1, 7)],
                         [(e.boot, e.code, e.uptime_ms) for e in events[:3]])
        boot2 = events[3:]
        self.assertEqual(list(range(70)), [e.seq for e in boot2])
        self.assertTrue(all(e.boot == 2 and e.code == 52 for e in boot2))
        self.assertEqual([1000 + 10 * n for n in range(70)], [e.uptime_ms for e in boot2])
        self.assertEqual(list(range(70)), [e.data[0] for e in boot2])
        self.assertTrue(all(e.crc_ok for e in events))

    def test_the_page_numbers_show_a_second_page_opened(self):
        self.assertEqual([1, 2], bl.event_log_page_numbers(sample_bank2()))

    def test_a_corrupt_event_reads_with_a_bad_crc(self):
        blob = bytearray(sample_bank2())
        first_event = bl.EVENT_LOG_OFFSET + 32  # after page 0's header
        blob[first_event + 4] ^= 0x01  # its uptime

        events = bl.parse_event_log(bytes(blob))
        self.assertFalse(events[0].crc_ok)
        self.assertTrue(events[1].crc_ok)

    def test_a_blank_bank_holds_no_events(self):
        self.assertEqual([], bl.parse_event_log(b"\xff" * len(sample_bank2())))


class ParameterCopies(unittest.TestCase):
    def test_reads_both_copies_with_their_save_counters(self):
        a, b = bl.parse_param_copies(sample_bank2())

        self.assertEqual(("good", 1, 1, {1: -7, 2: 900}), (a.status, a.image, a.save_counter, a.values))
        self.assertEqual(("good", 1, 2, {1: 42, 2: 900}), (b.status, b.image, b.save_counter, b.values))

    def test_the_set_in_force_is_the_newer_good_copy_of_the_image(self):
        self.assertEqual({1: 42, 2: 900}, bl.params_in_force(sample_bank2(), "vcu"))

    def test_the_other_images_copies_are_not_loaded(self):
        self.assertIsNone(bl.params_in_force(sample_bank2(), "bms"))

    def test_a_corrupted_copy_has_a_bad_crc_and_the_older_one_is_loaded(self):
        blob = bl.corrupt_param_copy(sample_bank2(), 1)

        self.assertEqual("bad CRC", bl.parse_param_copies(blob)[1].status)
        self.assertEqual({1: -7, 2: 900}, bl.params_in_force(blob, "vcu"))

    def test_both_copies_corrupted_leaves_no_set_in_force(self):
        blob = bl.corrupt_param_copy(bl.corrupt_param_copy(sample_bank2(), 0), 1)

        self.assertEqual(["bad CRC", "bad CRC"], [c.status for c in bl.parse_param_copies(blob)])
        self.assertIsNone(bl.params_in_force(blob, "vcu"))

    def test_corrupting_changes_nothing_outside_that_copy(self):
        before = sample_bank2()
        after = bl.corrupt_param_copy(before, 0)

        self.assertEqual(before[0x1000:], after[0x1000:])
        self.assertEqual(1, sum(x != y for x, y in zip(before, after)))

    def test_an_erased_copy_is_empty(self):
        self.assertEqual("empty", bl.parse_param_copy(b"\xff" * 0x1000).status)


def e2e_frames(can_id, times_s, counters):
    """Frames carrying an E2E Profile 11 counter in byte 1's low nibble."""
    return [bl.Frame(t, can_id, bytes([0, c, 0, 0, 0, 0, 0, 0])) for t, c in zip(times_s, counters)]


def inverter_frames(times_s, counters):
    """0x0C0: the rolling counter is bits 44-47, byte 5's high nibble."""
    return [bl.Frame(t, 0x0C0, bytes([0, 0, 0, 0, 0, c << 4, 0, 0])) for t, c in zip(times_s, counters)]


# The layouts as can/control.dbc has them today, written out here as the
# test's own source of truth: periods of the frames sent every step or
# every second step, and each counter's (start bit, length, values).
BMS = bl.BusLayout(periods_ms={0x080: 20.0, 0x082: 20.0},
                   counters={0x080: (8, 4, 15), 0x081: (8, 4, 15), 0x082: (8, 4, 15)})
VCU = bl.BusLayout(periods_ms={0x0C0: 10.0, 0x090: 20.0},
                   counters={0x090: (8, 4, 15), 0x0C0: (44, 4, 16)})


class ControlBusFrames(unittest.TestCase):
    def test_a_steady_bus_has_no_late_or_skipped_frame(self):
        frames = e2e_frames(0x080, [0.000, 0.020, 0.040, 0.060], [13, 14, 0, 1])

        j = bl.judge_control_bus(frames, BMS)
        self.assertEqual([], j.late)
        self.assertEqual([], j.skipped)
        self.assertAlmostEqual(20.0, j.worst_gap_ms[0x080])

    def test_a_gap_over_one_and_a_half_periods_is_late(self):
        # BMS_Status every 20 ms: 30 ms is the limit, 31 ms is late.
        frames = e2e_frames(0x080, [0.000, 0.030, 0.061], [0, 1, 2])

        j = bl.judge_control_bus(frames, BMS)
        self.assertEqual([(0x080, 0.061, 31.0)], [(i, t, round(g, 3)) for i, t, g in j.late])

    def test_a_counter_that_does_not_step_by_one_is_a_skipped_frame(self):
        frames = e2e_frames(0x090, [0.00, 0.02, 0.04], [14, 0, 2])

        self.assertEqual([(0x090, 0.04)], bl.judge_control_bus(frames, VCU).skipped)

    def test_the_inverter_command_counter_wraps_at_16(self):
        frames = inverter_frames([0.00, 0.01, 0.02, 0.03], [14, 15, 0, 2])

        self.assertEqual([(0x0C0, 0.03)], bl.judge_control_bus(frames, VCU).skipped)

    def test_frames_without_a_period_in_the_layout_are_not_judged_late(self):
        # BMS_Limits comes once in five steps: counted, not judged late.
        frames = e2e_frames(0x081, [0.0, 0.1, 0.3], [0, 1, 2])

        self.assertEqual([], bl.judge_control_bus(frames, BMS).late)

    def test_the_reset_is_the_first_gap_of_several_periods_after_the_mark(self):
        frames = e2e_frames(0x080, [0.00, 0.02, 0.04, 0.16, 0.18], [0, 1, 2, 0, 1])

        self.assertEqual((0.04, 0.16), bl.reset_gap(frames, 0x080, after_s=0.01, period_s=0.02))
        self.assertIsNone(bl.reset_gap(frames, 0x080, after_s=0.17, period_s=0.02))


def ev(uptime_ms, code=52):
    return bl.Event(0, 1, 1, code, uptime_ms, 0, bytes(16), True)


class FloodGuard(unittest.TestCase):
    def test_a_burst_of_64_then_one_a_second_is_within_the_guard(self):
        events = [ev(t) for t in range(64)] + [ev(1063 + 1000 * n) for n in range(5)]

        self.assertEqual([], bl.flood_excess(events))

    def test_66_events_in_half_a_second_exceed_it(self):
        events = [ev(t * 7) for t in range(66)]

        self.assertTrue(bl.flood_excess(events))

    def test_the_dropped_report_takes_no_token(self):
        events = ([ev(t) for t in range(64)] + [ev(100, bl.EVENT_EVENTS_DROPPED)]
                  + [ev(1063)])

        self.assertEqual([], bl.flood_excess(events))

    def test_spacing_after_the_burst_is_the_closest_pair_after_the_64th(self):
        events = [ev(t) for t in range(64)] + [ev(1063), ev(2050), ev(3060)]

        self.assertEqual(987, bl.spacing_after_burst(events))


class ShellText(unittest.TestCase):
    def test_step_gives_the_overrun_count(self):
        text = "step 4152, state 1\r\noverruns 3, timing faults 1, kicks withheld 0\r\n"
        self.assertEqual(3, bl.parse_overruns(text))

    def test_bus_gives_the_frames_skipped(self):
        text = ("CAN1  Control bus, classic: ERROR_ACTIVE\r\n"
                "     rx=10 rx_unknown=0 tx=5000 tx_skipped=2\r\n")
        self.assertEqual(2, bl.parse_tx_skipped(text))

    def test_lastreset_gives_the_cause_and_crash_record(self):
        text = ("last reset:   software (RCC_CSR 0x14000000)\r\n"
                "not power-on: 2 since the last power-on reset\r\n"
                "crash record: crash command in shellTask, at 0x08001234\r\n")
        self.assertEqual("software", bl.parse_last_reset(text))
        self.assertEqual("crash command in shellTask", bl.parse_crash_record(text))

    def test_lastreset_gives_the_breadcrumb(self):
        text = ("last reset:   watchdog (RCC_CSR 0x24000000)\r\n"
                "breadcrumb:   step 4152, in phase 2 (judge), 0 overruns\r\n")
        self.assertEqual("step 4152, in phase 2 (judge), 0 overruns", bl.parse_breadcrumb(text))

    def test_a_breadcrumb_with_no_step_is_none(self):
        self.assertIsNone(bl.parse_breadcrumb("breadcrumb:   no control step ran\r\n"))

    def test_no_crash_record(self):
        self.assertIsNone(bl.parse_crash_record("crash record: none\r\n"))

    def test_text_without_the_line_gives_none(self):
        self.assertIsNone(bl.parse_overruns("unknown command\r\n"))


def result(times, ok=None, budget_ms=100.0):
    runs = [bl.Run(t, True if ok is None else ok[i]) for i, t in enumerate(times)]
    return bl.Result(case="COM-01", title="Hang command", image="bms", image_commit="3c8cb03a",
                     board="TIM-07", budget="100 ms", budget_ms=budget_ms, runs=runs)


class Verdict(unittest.TestCase):
    def test_20_runs_at_or_under_the_budget_pass(self):
        r = result([80.0] * 19 + [100.0])
        self.assertEqual((100.0, "PASS"), (r.worst_ms, r.verdict))

    def test_one_run_over_the_budget_fails(self):
        self.assertEqual("FAIL", result([80.0] * 19 + [100.1]).verdict)

    def test_a_run_judged_wrong_fails_whatever_its_time(self):
        self.assertEqual("FAIL", result([80.0] * 20, ok=[True] * 19 + [False]).verdict)

    def test_fewer_than_20_runs_is_incomplete(self):
        self.assertEqual("INCOMPLETE", result([80.0] * 19).verdict)

    def test_a_case_with_no_budget_passes_on_its_runs(self):
        self.assertEqual("PASS", result([None] * 20, budget_ms=None).verdict)

    def test_a_failure_of_the_whole_case_fails_it_without_adding_a_run(self):
        r = result([80.0] * 20)
        r.case_failures.append("MCU_BMS_OK went high during a reset")

        self.assertEqual(("FAIL", 20), (r.verdict, len(r.times_ms)))

    def test_the_result_file_holds_what_the_plan_asks_for(self):
        r = result([float(n) for n in range(20)])
        with tempfile.TemporaryDirectory() as d:
            path = bl.write_result(r, d)
            with open(path, encoding="utf-8") as f:
                saved = json.load(f)

        self.assertTrue(os.path.basename(path).startswith("COM-01_bms_"))
        self.assertEqual("COM-01", saved["case"])
        self.assertEqual("3c8cb03a", saved["image_commit"])
        self.assertEqual("TIM-07", saved["board"])
        self.assertIn("date", saved)
        self.assertEqual([float(n) for n in range(20)], saved["times_ms"])
        self.assertEqual(19.0, saved["worst_ms"])
        self.assertEqual("PASS", saved["verdict"])


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: test_benchlib.py <dir holding bench_bank2.bin>")
    SAMPLE_DIR = sys.argv.pop(1)
    unittest.main()
