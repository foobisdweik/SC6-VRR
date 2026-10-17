#!/usr/bin/env python3
"""Mark a foreground battle interval, or export its Metal data after recording."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import time

spec = importlib.util.spec_from_file_location("measurements", Path(__file__).with_name("measurements.py"))
measurements = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measurements)


class ForegroundGate:
    def __init__(self, settle):
        self.settle = settle
        self.since = None
        self.expected = None

    def ready(self, now, expected, foreground):
        if expected != self.expected:
            self.expected, self.since = expected, None
        if not expected or foreground != expected:
            self.since = None
            return False
        if self.since is None:
            self.since = now
        return now - self.since >= self.settle


def foreground_pid():
    front = subprocess.run(["/usr/bin/lsappinfo", "front"], capture_output=True, text=True, check=True, timeout=5)
    info = subprocess.run(["/usr/bin/lsappinfo", "info", "-only", "pid", front.stdout.strip()],
                          capture_output=True, text=True, check=True, timeout=5)
    match = re.search(r"\bpid\s*=\s*(\d+)", info.stdout)
    return int(match[1]) if match else None


def current_game_pid(run_dir):
    path = run_dir / "game-pid.txt"
    if not path.exists() or time.time() - path.stat().st_mtime > 3:
        return None
    value = path.read_text().strip()
    if not value.isdigit():
        return None
    pid = int(value)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return None
    return pid


def marker(run_dir, name):
    stamp = time.time()
    with (run_dir / "markers.tsv").open("a") as output:
        output.write(f"{stamp:.6f}\t{name}\n")
    return stamp


def arm_window(run_dir, seconds, settle, timeout, scene=None, on_poll=None):
    if not run_dir.is_dir():
        raise ValueError("The run directory does not exist. Start the harness first.")
    lock = run_dir / ".record-window.lock"
    with lock.open("x"):
        pass
    status = dict(valid=False, requested_seconds=seconds, settle_seconds=settle,
                  scene=scene or "not recorded")
    started = False
    try:
        gate = ForegroundGate(settle)
        limit = time.monotonic() + timeout
        print("Armed. Return to the game. Recording starts after the game remains frontmost for the settling period.", flush=True)
        while True:
            if time.monotonic() >= limit:
                raise RuntimeError("The game did not remain frontmost before the timeout. No complete interval was recorded.")
            pid = current_game_pid(run_dir)
            if gate.ready(time.monotonic(), pid, foreground_pid()):
                break
            time.sleep(0.5)
        status.update(pid=pid, start_unix=marker(run_dir, "battle-start"))
        started = True
        deadline = time.monotonic() + seconds
        print(f"Recording {seconds:g} seconds. Keep the game frontmost.", flush=True)
        with (run_dir / "focus.tsv").open("a") as output:
            while time.monotonic() < deadline:
                actual = foreground_pid()
                output.write(f"{time.time():.6f}\t{pid}\t{actual}\n")
                output.flush()
                if current_game_pid(run_dir) != pid or actual != pid:
                    raise RuntimeError("The game lost the foreground or its verified process changed. This interval was aborted.")
                if on_poll is not None:
                    on_poll(pid, status["start_unix"])
                time.sleep(min(0.5, max(0, deadline - time.monotonic())))
        # Verify the final boundary instead of assuming focus stayed unchanged.
        if current_game_pid(run_dir) != pid or foreground_pid() != pid:
            raise RuntimeError("The game lost the foreground at the final boundary. This interval was aborted.")
        status.update(valid=True, end_unix=marker(run_dir, "battle-end"))
        print("The battle interval is complete. You can quit the game.", flush=True)
    except Exception as error:
        status["error"] = str(error)
        if started:
            marker(run_dir, "battle-abort")
        raise
    finally:
        (run_dir / "window.json").write_text(json.dumps(status, indent=2) + "\n")
        lock.unlink(missing_ok=True)


def collect_metal(run_dir):
    intervals = measurements.battle_intervals(run_dir)
    pid_path = run_dir / "game-pids.txt"
    pids = [int(value) for value in pid_path.read_text().split()] if pid_path.exists() else []
    status = dict(exported=False, validated_gpu_metrics=False)
    if not intervals or not pids:
        status["error"] = "No complete battle interval or verified game process exists. Metal export was skipped."
    else:
        output = run_dir / "metal-traces"
        output.mkdir(exist_ok=True)
        command = ["/usr/bin/metalperftrace", "collect", "--start", f"@{intervals[0][0]:.6f}",
                   "--end", f"@{intervals[-1][1]:.6f}", "--prefix", "SC6", "--json", str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=45)
        (run_dir / "metal-collect.json").write_text(result.stdout)
        (run_dir / "metal-capture.log").write_text(result.stderr)
        status.update(collect_exit=result.returncode, intervals=intervals, pids=pids, exports=[])
        if result.returncode == 0:
            predicate = " OR ".join(f"pid == {pid}" for pid in pids)
            for trace in sorted(output.rglob("*.atrc")):
                summary = subprocess.run(["/usr/bin/metalperftrace", "overview", "--json-include-timeline",
                                          "--predicate", predicate, str(trace)],
                                         capture_output=True, text=True, timeout=45)
                destination = trace.with_suffix(".overview.json")
                destination.write_text(summary.stdout)
                trace.with_suffix(".overview.log").write_text(summary.stderr)
                status["exports"].append(dict(path=str(destination), exit_code=summary.returncode))
            status["exported"] = bool(status["exports"]) and all(item["exit_code"] == 0 for item in status["exports"])
        if not status["exported"]:
            status["error"] = "Metal export produced no complete overview. Preserve the raw files and inspect the capture log."
    # Export success does not establish that GPU timing fields are present.
    # Inspect the native schema after the first live run before deriving rates.
    (run_dir / "metal-status.json").write_text(json.dumps(status, indent=2) + "\n")
    print(json.dumps(status, indent=2))


def preflight():
    repo = Path(__file__).resolve().parent.parent
    game = Path(os.environ.get("SC6_EXE", str(Path.home() / "Games/SteamLibrary/steamapps/common/SoulCalibur 6/SoulcaliburVI/Binaries/Win64/SoulcaliburVI.exe")))
    import hashlib
    expected_exe = "f8904e4b04bca3b47bc52a683f6190365d2eb89ee8f44f8072759e9c5e04a553"
    if hashlib.sha256(game.read_bytes()).hexdigest() != expected_exe:
        raise RuntimeError("The game executable differs from the supported build.")
    built = (repo / "dll/build/dinput8.dll").read_bytes()
    if built != game.with_name("dinput8.dll").read_bytes() or b"frame_detail_v=1" not in built:
        raise RuntimeError("The installed DLL does not match the prepared frame-timer build.")
    for tool in ("/usr/bin/metalperftrace", "/usr/bin/lsappinfo"):
        if not os.access(tool, os.X_OK):
            raise RuntimeError(f"Required tool is missing: {tool}")
    print("Preflight passed. The prepared DLL is installed. No game was launched and no recording was started.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("arm", "collect-metal", "preflight"))
    parser.add_argument("run_dir", nargs="?", type=Path)
    parser.add_argument("--seconds", type=float, default=90)
    parser.add_argument("--settle", type=float, default=5)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--scene", help="Record the Training stage, characters, camera, and action pattern.")
    args = parser.parse_args()
    if args.seconds <= 0 or args.settle < 0 or args.timeout <= 0:
        parser.error("Choose a positive recording duration and timeout, and a nonnegative settling period.")
    if args.action == "preflight":
        preflight()
    elif args.run_dir is None:
        parser.error("Choose the harness run directory.")
    elif args.action == "arm":
        arm_window(args.run_dir, args.seconds, args.settle, args.timeout, args.scene)
    else:
        collect_metal(args.run_dir)


if __name__ == "__main__":
    main()
