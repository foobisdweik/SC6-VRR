"""Check capture boundaries without starting a game or collecting Metal data."""

import ctypes
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("record_window", Path(__file__).with_name("record_window.py"))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)
m = r.measurements


class CaptureTest(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "darwin", "Requires the Darwin libproc API")
    def test_native_thread_ids_match_the_os_and_cpu_clock(self):
        lib = ctypes.CDLL("/usr/lib/libproc.dylib")
        tid = threading.get_native_id()
        before = {row["native_thread_id"]: row["cpu_time_s"]
                  for row in m.thread_snapshot(lib, os.getpid(), native_ids=True)}
        self.assertIn(tid, before)
        clock = time.thread_time()
        end = time.monotonic() + 0.03
        while time.monotonic() < end:
            pass
        used = time.thread_time() - clock
        after = {row["native_thread_id"]: row["cpu_time_s"]
                 for row in m.thread_snapshot(lib, os.getpid(), native_ids=True)}
        self.assertAlmostEqual(after[tid] - before[tid], used, delta=0.02)

    def test_foreground_must_remain_stable(self):
        gate = r.ForegroundGate(5)
        self.assertFalse(gate.ready(0, 10, 20))
        self.assertFalse(gate.ready(1, 10, 10))
        self.assertFalse(gate.ready(5, 10, 10))
        self.assertTrue(gate.ready(6, 10, 10))
        self.assertFalse(gate.ready(7, 10, 20))
        self.assertFalse(gate.ready(8, 10, 10))
        self.assertFalse(gate.ready(12, 10, 10))
        self.assertTrue(gate.ready(13, 10, 10))
        self.assertFalse(gate.ready(14, 11, 11))

    def test_aborted_interval_is_not_used(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "markers.tsv").write_text("100\tbattle-start\n105\tbattle-abort\n110\tbattle-end\n120\tbattle-start\n150\tbattle-end\n")
            self.assertEqual(m.battle_intervals(root), [(120, 150)])

    def test_metal_export_is_skipped_without_valid_boundaries(self):
        with tempfile.TemporaryDirectory() as temp, mock.patch.object(r.subprocess, "run") as run:
            r.collect_metal(Path(temp))
            run.assert_not_called()
            self.assertIn('"exported": false', (Path(temp) / "metal-status.json").read_text())

    def test_metal_export_filters_the_verified_game_process(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "markers.tsv").write_text("100\tbattle-start\n145\tbattle-end\n")
            (root / "game-pids.txt").write_text("123\n")
            trace = root / "metal-traces" / "example.atrc"
            calls = []

            def fake_run(command, **kwargs):
                calls.append(command)
                if command[1] == "collect":
                    trace.write_text("fixture")
                    return mock.Mock(returncode=0, stdout='{"traces": []}', stderr="")
                return mock.Mock(returncode=0, stdout='{"fixture_only": true}', stderr="")

            with mock.patch.object(r.subprocess, "run", side_effect=fake_run):
                r.collect_metal(root)
            self.assertIn("@100.000000", calls[0])
            self.assertIn("@145.000000", calls[0])
            self.assertIn("pid == 123", calls[1])
            self.assertIn('"validated_gpu_metrics": false', (root / "metal-status.json").read_text())

    def test_detail_summary_keeps_overlapping_scopes_separate(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / "sc6vrr.log"
            values = " ".join(f"{name}_total_ms={total} {name}_max_ms=10" for name, total in
                              [("loop", 1200), ("game", 800), ("world", 200), ("sync", 300), ("fence", 250)])
            p.write_text(f"frame_detail_v=1 unix_s=102 elapsed_s=1 frames=100 sync_in_game=0 {values}\n")
            rows = m.detail_windows(p, [(101, 102)])
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["loop_total_ms"] / rows[0]["frames"], 12)
            self.assertEqual(m.detail_windows(p, [(101.1, 102)]), [])

    def test_thread_cpu_time_is_measured_per_thread(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp) / "threads.tsv"
            p.write_text("unix_s\tmonotonic_s\tpid\tthread_key\tname\tcpu_time_s\n"
                         "100\t10\t5\t0x1\tdxmt-encode-thr\t1\n"
                         "102\t12\t5\t0x1\tdxmt-encode-thr\t2.5\n"
                         "100\t10\t5\t0x2\t\t3\n102\t12\t5\t0x2\t\t3.2\n")
            rows = m.summarize_threads(p, [(100, 102)])
            self.assertEqual(rows[0]["name"], "dxmt-encode-thr")
            self.assertEqual(rows[0]["cpu_percent"], 75)
            self.assertEqual(rows[1]["name"], "unnamed")
            self.assertAlmostEqual(rows[1]["cpu_percent"], 10)

    @unittest.skipUnless(sys.platform == "darwin", "Requires the Darwin libproc API")
    def test_native_thread_cpu_counter_units(self):
        lib = ctypes.CDLL("/usr/lib/libproc.dylib")
        self.assertEqual(ctypes.sizeof(m.ProcThreadInfo), 112)
        before = {x["thread_key"]: x["cpu_time_s"] for x in m.thread_snapshot(lib, os.getpid())}
        cpu = time.thread_time()
        limit = time.monotonic() + 0.03
        while time.monotonic() < limit:
            pass
        expected = time.thread_time() - cpu
        after = m.thread_snapshot(lib, os.getpid())
        deltas = [x["cpu_time_s"] - before[x["thread_key"]] for x in after if x["thread_key"] in before]
        self.assertTrue(deltas)
        self.assertGreater(max(deltas), 0)
        self.assertAlmostEqual(max(deltas), expected, delta=0.02)


if __name__ == "__main__":
    unittest.main()
