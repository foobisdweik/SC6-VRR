"""Check measurements that previously produced misleading performance summaries."""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("measurements", Path(__file__).with_name("measurements.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class MeasurementsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def file(self, name, text):
        path = self.root / name
        path.write_text(text)
        return path

    def hud(self, stamp, count, pid=77478):
        return f"2026-10-15 {stamp} Df wineloader[{pid}:c324a] [com.apple.metal.hud:Logging] metal-HUD: {count},709.48,1734.84<private>\n"

    def test_legacy_cpu_keeps_idle_wrapper_separate(self):
        path = self.file("proc.tsv", "time\tpid\tcpu_pct\trss_kb\n15:00:00\t11\t0.3\t6000\n15:00:00\t22\t195.1\t2000000\n")
        rows = m.summarize_cpu(path)
        self.assertEqual([r["observed_cpu_percent"] for r in rows], [0.3, 195.1])
        self.assertTrue(all(r["cpu_percent"] is None for r in rows))
        self.assertTrue(all(r["role"] == "unverified" for r in rows))

    def test_cpu_uses_cpu_time_and_elapsed_time(self):
        path = self.file("proc.tsv", "unix_s\tmonotonic_s\tpid\tcpu_pct\trss_kb\tcpu_time_s\trole\n"
                         "100\t10\t22\t80\t1000\t2\tgame\n"
                         "101\t11\t22\t80\t1000\t4\tgame\n"
                         "104\t14\t22\t80\t1000\t7\tgame\n")
        row = m.summarize_cpu(path)[0]
        self.assertEqual(row["cpu_percent"], 125)
        self.assertEqual(row["measured_s"], 4)
        battle = m.summarize_cpu(path, [(101, 104)])[0]
        self.assertEqual(battle["cpu_percent"], 100)
        self.assertEqual(battle["measured_s"], 3)

    def test_cpu_counter_decrease_is_not_negative_cpu(self):
        path = self.file("proc.tsv", "unix_s\tmonotonic_s\tpid\tcpu_pct\trss_kb\tcpu_time_s\n"
                         "100\t10\t22\t80\t1000\t9\n101\t11\t22\t80\t1000\t1\n")
        self.assertIsNone(m.summarize_cpu(path)[0]["cpu_percent"])

    def test_wine_path_change_retains_verified_process_identity(self):
        previous = dict(role="game", args="SoulcaliburVI.exe ", cpu_time_s="10")
        renamed = dict(role="candidate", args="SoulcaliburVI.exe ", cpu_time_s="12")
        self.assertTrue(m.retains_identity(renamed, previous))
        self.assertFalse(m.retains_identity(dict(renamed, cpu_time_s="1"), previous))
        self.assertFalse(m.retains_identity(dict(renamed, args="start.exe SoulcaliburVI.exe"), previous))
        self.assertFalse(m.retains_identity(renamed, dict(previous, role="candidate")))
        path = self.file("proc.tsv", "unix_s\tmonotonic_s\tpid\tcpu_pct\trss_kb\tcpu_time_s\trole\targs\n"
                         "100\t10\t22\t100\t1000\t10\tgame\tSoulcaliburVI.exe\n"
                         "101\t11\t22\t100\t1000\t12\tcandidate\tSoulcaliburVI.exe\n"
                         "102\t12\t22\t100\t1000\t14\tcandidate\tSoulcaliburVI.exe\n")
        row = m.summarize_cpu(path, [(101, 102)])[0]
        self.assertEqual(row["role"], "game")
        self.assertTrue(row["verification_retained"])
        self.assertEqual(row["cpu_percent"], 200)

    def test_ps_cpu_time_formats_and_arguments_with_spaces(self):
        self.assertEqual(m.cpu_seconds("2-01:02:03.50"), 176523.5)
        self.assertEqual(m.cpu_seconds("3:19.19"), 199.19)
        row = list(m.process_rows(' 22 1 195.1 2048 3:19.19 Y:\\Game Folder\\SoulcaliburVI.exe -arg'))[0]
        self.assertEqual(row["args"], r"Y:\Game Folder\SoulcaliburVI.exe -arg")

    def test_target_selection_excludes_launcher_arguments(self):
        self.assertTrue(m.is_target_command(r'Y:\Game Folder\SoulcaliburVI.exe -arg', "SoulcaliburVI.exe"))
        self.assertTrue(m.is_target_command(r'"Y:\Game Folder\SoulcaliburVI.exe" -arg', "SoulcaliburVI.exe"))
        self.assertFalse(m.is_target_command(r'start.exe Y:\Game Folder\SoulcaliburVI.exe', "SoulcaliburVI.exe"))
        self.assertFalse(m.is_target_command(r'winewrapper.exe --start SoulcaliburVI.exe', "SoulcaliburVI.exe"))

    def test_real_hud_zero_restart_does_not_create_4754_fps(self):
        path = self.file("metal.log", self.hud("16:19:30.476", 100)
                         + self.hud("16:19:31.476", 0)
                         + self.hud("16:19:32.413", 4455)
                         + self.hud("16:19:33.413", 4515))
        group = m.summarize_hud(path)["77478"]
        self.assertEqual(group["accepted"], [(1, 60)])
        self.assertEqual(group["rejected"]["zero counter"], 1)

    def test_hud_keeps_processes_separate(self):
        path = self.file("metal.log", self.hud("16:00:00.000", 10, 1)
                         + self.hud("16:00:00.500", 5000, 2)
                         + self.hud("16:00:01.000", 70, 1)
                         + self.hud("16:00:01.500", 5060, 2))
        groups = m.summarize_hud(path)
        self.assertEqual(groups["1"]["accepted"], [(1, 60)])
        self.assertEqual(groups["2"]["accepted"], [(1, 60)])
        self.assertEqual(set(m.summarize_hud(path, allowed_pids={"2"})), {"2"})

    def test_hud_average_is_weighted_by_elapsed_time(self):
        path = self.file("metal.log", self.hud("16:00:00.000", 10)
                         + self.hud("16:00:01.000", 70)
                         + self.hud("16:00:04.000", 130))
        values = m.summarize_hud(path)["77478"]["accepted"]
        self.assertEqual(sum(f for _, f in values) / sum(s for s, _ in values), 30)

    def test_hud_rejects_counter_jumps_and_decreases(self):
        path = self.file("metal.log", self.hud("16:00:00.000", 10)
                         + self.hud("16:00:01.000", 5000)
                         + self.hud("16:00:02.000", 20)
                         + self.hud("16:00:03.000", 80))
        group = m.summarize_hud(path)["77478"]
        self.assertEqual(group["accepted"], [(1, 60)])
        self.assertEqual(dict(group["rejected"]), {"above ceiling": 1, "counter decrease": 1})

    def test_hud_does_not_move_baseline_backwards(self):
        path = self.file("metal.log", self.hud("16:00:01.000", 100)
                         + self.hud("16:00:00.000", 40)
                         + self.hud("16:00:02.000", 160))
        group = m.summarize_hud(path)["77478"]
        self.assertEqual(group["accepted"], [(1, 60)])

    def test_hud_handles_midnight(self):
        path = self.file("metal.log", self.hud("23:59:59.500", 10)
                         + self.hud("00:00:00.500", 70).replace("2026-10-15", "2026-10-16"))
        self.assertEqual(m.summarize_hud(path, utc_offset="-0700")["77478"]["accepted"], [(1, 60)])

    def test_markers_require_a_complete_interval(self):
        self.file("markers.tsv", "99\tbattle-end\n100\tbattle-start\n104\tbattle-end\n105\tbattle-start\n")
        self.assertEqual(m.battle_intervals(self.root), [(100, 104)])
        self.assertFalse(m.inside(99, 101, [(100, 104)]))

    def test_dll_does_not_treat_legacy_decisions_as_completed_steps(self):
        stats = " counter_resets=0 clock_r1=60 clock_r2=0 world_calls=120 world_total_ms=600 world_max_ms=8 engine_time_total_ms=200 engine_time_max_ms=4\n"
        path = self.file("sc6vrr.log", "16:00:00 frames=120 steps=60 (1.0 s)\n"
                         + "16:00:01 metrics_v=1 unix_s=101 elapsed_s=1.0 engine_frames=120 battle_completed=57" + stats
                         + "16:00:02 metrics_v=1 unix_s=103 elapsed_s=2.0 engine_frames=90 battle_completed=89" + stats
                         + "16:00:03 metrics_v=1 unix_s=104 elapsed_s=1.0 engine_frames=120 battle_completed=60\n")
        rows = m.dll_windows(path)
        self.assertEqual(len(rows), 2)
        self.assertEqual(sum(r["battle_completed"] for r in rows), 146)
        self.assertEqual(len(m.dll_windows(path, [(100, 101)])), 1)

    def test_summary_warns_when_no_game_process_was_verified(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            m.print_summary(self.root, "cpu", None, 240)
        self.assertIn("No game process was verified", output.getvalue())

    def test_explicit_interval_does_not_include_the_whole_run(self):
        output = io.StringIO()
        argv = ["measurements.py", "summary", str(self.root), "--from-unix", "100", "--to-unix", "102"]
        with mock.patch("sys.argv", argv), contextlib.redirect_stdout(output):
            m.main()
        self.assertIn("Selected UTC interval", output.getvalue())
        self.assertNotIn("Whole run", output.getvalue())


if __name__ == "__main__":
    unittest.main()
