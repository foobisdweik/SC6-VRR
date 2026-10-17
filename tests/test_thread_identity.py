import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("identity", Path(__file__).with_name("thread_identity.py"))
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class IdentityTests(unittest.TestCase):
    def test_profiler_environment_uses_an_allowlist_and_rejects_credentials(self):
        source = {key: "fixture" for key in (
            "PATH", "HOME", "LC_CTYPE", "SC6_BOTTLE", "CROSSOVER_APP", "HF_TOKEN",
            "SC6_API_KEY", "CROSSOVER_PASSWORD", "PRIVATE_VALUE", "CODEX_THREAD_ID")}
        self.assertEqual(set(identity.clean_environment(source)),
                         {"PATH", "HOME", "LC_CTYPE", "SC6_BOTTLE", "CROSSOVER_APP"})

    def test_ranking_keeps_unnamed_os_threads_separate(self):
        rows = []
        for tid, used in ((123, 6), (456, 2)):
            rows.extend([
                dict(native_thread_id=tid, name="", unix_s="100", monotonic_s="10", cpu_time_s=1),
                dict(native_thread_id=tid, name="", unix_s="106", monotonic_s="16", cpu_time_s=1 + used),
            ])
        ranked = identity.ranking(rows, 99)
        self.assertEqual([row["native_thread_id"] for row in ranked], [123, 456])
        self.assertEqual(ranked[0]["cpu_percent"], 100)
        self.assertAlmostEqual(ranked[1]["cpu_percent"], 100 / 3)
        self.assertEqual(identity.ranking(rows, 101), [])

    def test_ranking_rejects_a_decreasing_cpu_counter(self):
        rows = [dict(native_thread_id=123, name="", unix_s=str(100 + i * 6),
                     monotonic_s=str(10 + i * 6), cpu_time_s=5 - i) for i in range(2)]
        self.assertEqual(identity.ranking(rows, 99), [])


if __name__ == "__main__":
    unittest.main()
