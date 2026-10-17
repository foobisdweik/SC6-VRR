#!/usr/bin/env python3
"""Join CPU counters to an OS thread ID, then capture its executing functions."""

import argparse
import csv
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

spec = importlib.util.spec_from_file_location("record_window", Path(__file__).with_name("record_window.py"))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)
m = r.measurements


def clean_environment(source=None):
    source = os.environ if source is None else source
    allowed = {"PATH", "HOME", "USER", "LOGNAME", "SHELL", "TMPDIR", "LANG", "DEVELOPER_DIR"}
    return {key: value for key, value in source.items()
            if not re.search(r"TOKEN|SECRET|PASSWORD|CREDENTIAL|API_KEY", key, re.I)
            and (key in allowed or key.startswith(("LC_", "SC6_", "CROSSOVER_")))}


def ranking(rows, since):
    groups = {}
    for row in rows:
        if float(row["unix_s"]) >= since:
            groups.setdefault(row["native_thread_id"], []).append(row)
    result = []
    for tid, values in groups.items():
        first, last = values[0], values[-1]
        elapsed = float(last["monotonic_s"]) - float(first["monotonic_s"])
        cpu = last["cpu_time_s"] - first["cpu_time_s"]
        if elapsed >= 5 and cpu >= 0:
            result.append(dict(native_thread_id=tid, name=last["name"] or "unnamed",
                               elapsed_s=elapsed, cpu_percent=100 * cpu / elapsed))
    return sorted(result, key=lambda row: row["cpu_percent"], reverse=True)


def arm(run):
    lib = ctypes.CDLL("/usr/lib/libproc.dylib")
    rows = []
    status = {"purpose": "Thread identification only. Do not compare its fps.", "capture_started": False}
    child = None
    log = None
    last_read = 0
    destination = run / "thread-identity.trace"
    if destination.exists():
        raise RuntimeError("This run already contains a profiler trace. Do not overwrite it.")
    with (run / "native-threads.tsv").open("x") as output:
        writer = csv.DictWriter(output, delimiter="\t", fieldnames=(
            "pid", "native_thread_id", "thread_key", "name", "cpu_time_s", "unix_s", "monotonic_s"))
        writer.writeheader()

        def poll(pid, start):
            nonlocal child, log, last_read
            now = time.time()
            if now - last_read < 1:
                return
            last_read = now
            snapshot = m.thread_snapshot(lib, pid, native_ids=True)
            writer.writerows(snapshot)
            output.flush()
            rows.extend(snapshot)
            if child is not None or now - start < 55:
                return
            measurements = m.dll_windows(run / "sc6vrr.log", [(start, now)])
            active_since = None
            for row in measurements:
                if row["battle_completed"] <= 0 or row["counter_resets"]:
                    active_since = None
                elif active_since is None:
                    active_since = row["unix_s"]
            if not measurements or now - measurements[-1]["unix_s"] > 3:
                return
            if active_since is None or now - active_since < 18:
                return
            if now - start > 75:
                return
            candidates = ranking(rows, now - 10)
            if not candidates:
                return
            status.update(pid=pid, pre_capture_ranking=candidates, target=candidates[0],
                          command_start_unix=now, capture_started=True)
            r.marker(run, "thread-profile-command-start")
            log = (run / "thread-profiler.log").open("x")
            child = subprocess.Popen([
                "xcrun", "xctrace", "record", "--template", "Time Profiler", "--attach", str(pid),
                "--time-limit", "8s", "--no-prompt", "--output", str(destination)],
                stdout=log, stderr=subprocess.STDOUT, env=clean_environment())
            print(f"Identifying OS thread {candidates[0]['native_thread_id']}. The profiler is starting.", flush=True)

        try:
            r.arm_window(run, 90, 5, 180,
                         "Kaer Morhen; Geralt and Groh; both fighters idle; diagnostic capture", on_poll=poll)
            if child is None:
                raise RuntimeError("No settled gameplay interval was available for the profiler. No thread was identified.")
            status["profiler_exit"] = child.wait(timeout=30)
            status["command_end_unix"] = time.time()
            r.marker(run, "thread-profile-command-end")
            if status["profiler_exit"]:
                raise RuntimeError("Time Profiler failed. Inspect thread-profiler.log. Do not infer a thread match.")
            status["trace_recorded"] = True
            for schema in ("time-sample", "thread-info"):
                # The index contains environment values. Export only the requested data table.
                with (run / f"{schema}.xml").open("x") as exported, (run / f"{schema}-export.log").open("x") as errors:
                    result = subprocess.run([
                        "xcrun", "xctrace", "export", "--input", str(destination), "--xpath",
                        f'/trace-toc/run[@number="1"]/data/table[@schema="{schema}"]'],
                        stdout=exported, stderr=errors, env=clean_environment(), timeout=60)
                    status[f"{schema}_export_exit"] = result.returncode
            print("The thread trace is saved. You can quit SC6.", flush=True)
        except Exception as error:
            status["error"] = str(error)
            if child is not None and child.poll() is None:
                child.send_signal(signal.SIGINT)
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            raise
        finally:
            if log is not None:
                log.close()
            (run / "thread-identity.json").write_text(json.dumps(status, indent=2) + "\n")


def arm_names(run):
    lib = ctypes.CDLL("/usr/lib/libproc.dylib")
    rows = []
    last_read = 0
    with (run / "native-threads.tsv").open("x") as output:
        writer = csv.DictWriter(output, delimiter="\t", fieldnames=(
            "pid", "native_thread_id", "thread_key", "name", "cpu_time_s", "unix_s", "monotonic_s"))
        writer.writeheader()

        def poll(pid, start):
            nonlocal last_read
            if time.time() - last_read < 1:
                return
            last_read = time.time()
            snapshot = m.thread_snapshot(lib, pid, native_ids=True)
            writer.writerows(snapshot)
            output.flush()
            rows.extend(snapshot)

        r.arm_window(run, 90, 5, 180,
                     "Kaer Morhen; Geralt and Groh; both idle; legacy thread names observed", on_poll=poll)
    window = json.loads((run / "window.json").read_text())
    result = dict(purpose="Read game thread names and CPU counters without stack sampling.",
                  pid=window["pid"], selection_unix=[window["end_unix"] - 35, window["end_unix"]],
                  ranking=ranking(rows, window["end_unix"] - 35))
    (run / "thread-names-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["ranking"][:4], indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("launch", "arm", "names"))
    parser.add_argument("run_dir", nargs="?", type=Path)
    parser.add_argument("--case", choices=("18-thread-identity", "19-thread-names"), default="18-thread-identity")
    args = parser.parse_args()
    if args.action == "launch":
        script = Path(__file__).with_name("run.sh").resolve()
        os.execve(str(script), [str(script), args.case], clean_environment())
    elif args.run_dir is None:
        parser.error("Choose the diagnostic run directory.")
    elif args.action == "arm":
        arm(args.run_dir)
    else:
        arm_names(args.run_dir)


if __name__ == "__main__":
    main()
