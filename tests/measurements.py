#!/usr/bin/env python3
"""Collect process observations and summarize measurements without mixing processes."""

import argparse
import csv
import ctypes
import datetime as dt
import os
import ntpath
from pathlib import Path
import re
import subprocess
import time
from collections import defaultdict


def cpu_seconds(value):
    """Convert the cumulative CPU time printed by macOS ps to seconds."""
    days, _, clock = value.rpartition("-")
    parts = [float(x) for x in clock.split(":")]
    if len(parts) not in (2, 3):
        raise ValueError(value)
    result = parts[-1] + parts[-2] * 60
    if len(parts) == 3:
        result += parts[0] * 3600
    return result + (int(days) * 86400 if days else 0)


def process_rows(text):
    for line in text.splitlines():
        fields = line.split(None, 5)
        if len(fields) != 6:
            continue
        try:
            pid, parent, cpu, rss, clock, command = fields
            yield dict(pid=int(pid), parent=int(parent), cpu_pct=float(cpu),
                       rss_kb=int(rss), cpu_time_s=cpu_seconds(clock), args=command)
        except ValueError:
            continue


def is_target_command(command, name):
    """Match the program being run, not a target named in a launcher's arguments."""
    command = command.strip()
    if command.startswith(('"', "'")):
        executable = command.split(command[0], 2)[1]
    else:
        match = re.match(r".*?\.exe(?=\s|$)", command, re.I)
        executable = match[0] if match else command.split(None, 1)[0]
    return ntpath.basename(executable).casefold().removesuffix(".exe") == name.casefold().removesuffix(".exe")


def retains_identity(row, previous):
    """Retain verification while the command and cumulative CPU time stay continuous."""
    return bool(previous and previous.get("role") == "game"
                and row.get("args", "").strip() == previous.get("args", "").strip()
                and "cpu_time_s" in row and "cpu_time_s" in previous
                and float(row["cpu_time_s"]) >= float(previous["cpu_time_s"]))


class ProcThreadInfo(ctypes.Structure):
    # This layout follows the installed SDK's sys/proc_info.h.
    _fields_ = [("user_ns", ctypes.c_uint64), ("system_ns", ctypes.c_uint64)] + [
        (name, ctypes.c_int32) for name in
        ("cpu_usage", "policy", "run_state", "flags", "sleep_time", "current_priority", "priority", "maximum_priority")
    ] + [("name", ctypes.c_char * 64)]


def thread_snapshot(libproc, pid, native_ids=False):
    """Read cumulative thread CPU counters. This does not request stack samples."""
    libproc.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
    libproc.proc_pidinfo.restype = ctypes.c_int
    handles = (ctypes.c_uint64 * 4096)()
    # Flavor 28 returns OS thread IDs. Flavor 6 returns libproc handles.
    # The matching lookup flavors are 15 and 5, respectively.
    size = libproc.proc_pidinfo(pid, 28 if native_ids else 6, 0, handles, ctypes.sizeof(handles))
    if native_ids and size <= 0:
        raise RuntimeError("Native thread IDs could not be read. Do not infer a thread match.")
    if size == ctypes.sizeof(handles):
        raise RuntimeError("The thread list filled its buffer. Thread measurements may be incomplete.")
    rows = []
    for handle in handles[:max(0, size) // ctypes.sizeof(ctypes.c_uint64)]:
        info = ProcThreadInfo()
        if libproc.proc_pidinfo(pid, 15 if native_ids else 5, handle, ctypes.byref(info), ctypes.sizeof(info)) != ctypes.sizeof(info):
            continue  # A thread can exit between the two queries.
        rows.append(dict(pid=pid, thread_key=hex(handle), name=info.name.decode(errors="replace"),
                         cpu_time_s=(info.user_ns + info.system_ns) / 1e9,
                         unix_s=f"{time.time():.6f}", monotonic_s=f"{time.monotonic():.6f}"))
        if native_ids:
            rows[-1]["native_thread_id"] = handle
    return rows


def collect(run_dir):
    """Keep every matching process separate. Verify Wine processes by native path."""
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib")
    libproc.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    libproc.proc_pidpath.restype = ctypes.c_int
    root = Path(os.environ["SC6_PROCESS_CX_ROOT"])
    expected = {str(p.resolve()) for pattern in
                ("lib/wine/*-unix/wine", "lib/wine/*-unix/wine.app/Contents/MacOS/wine")
                for p in root.glob(pattern)}
    pattern = re.compile(os.environ.get("SC6_PROCESS_PATTERN", r"SoulcaliburVI\.exe"), re.I)
    name = os.environ.get("SC6_PROCESS_NAME", "SoulcaliburVI.exe")
    known_pids = set()
    previous_rows = {}
    fields = ["unix_s", "monotonic_s", "pid", "cpu_pct", "rss_kb", "cpu_time_s",
              "role", "native_path", "args"]
    measure_threads = os.environ.get("SC6_THREAD_METRICS") == "1"
    with (run_dir / "proc.tsv").open("w") as output, (run_dir / "threads.tsv").open("w") as thread_output:
        writer = csv.DictWriter(output, fields, delimiter="\t")
        writer.writeheader()
        thread_writer = csv.DictWriter(thread_output, ["unix_s", "monotonic_s", "pid", "thread_key", "name", "cpu_time_s"], delimiter="\t")
        thread_writer.writeheader()
        while True:
            started = time.monotonic()
            result = subprocess.run(["ps", "-axo", "pid=,ppid=,pcpu=,rss=,time=,args="],
                                    capture_output=True, text=True, check=True)
            stamp, mono = time.time(), time.monotonic()
            game_pids = []
            current_rows = {}
            for row in process_rows(result.stdout):
                if row["pid"] == os.getpid() or not pattern.search(row["args"]):
                    continue
                path = ctypes.create_string_buffer(4096)
                found = libproc.proc_pidpath(row["pid"], path, len(path))
                native = os.fsdecode(path.value) if found > 0 else ""
                target = is_target_command(row["args"], name)
                verified = target and ((native and str(Path(native).resolve()) in expected)
                                       or retains_identity(row, previous_rows.get(row["pid"])))
                role = "game" if verified else "candidate"
                if role == "game":
                    game_pids.append(row["pid"])
                    known_pids.add(row["pid"])
                row.pop("parent")
                row["role"] = role
                current_rows[row["pid"]] = row
                writer.writerow(dict(row, unix_s=f"{stamp:.6f}", monotonic_s=f"{mono:.6f}",
                                     role=role, native_path=native))
                if role == "game" and measure_threads:
                    for thread in thread_snapshot(libproc, row["pid"]):
                        thread_writer.writerow(thread)
            output.flush()
            thread_output.flush()
            previous_rows = current_rows
            (run_dir / "game-pids.txt").write_text("".join(f"{p}\n" for p in sorted(known_pids)))
            current = run_dir / "game-pid.txt"
            if len(game_pids) == 1:
                temporary = run_dir / ".game-pid.tmp"
                temporary.write_text(f"{game_pids[0]}\n")
                temporary.replace(current)
            else:
                current.unlink(missing_ok=True)
            time.sleep(max(0, 1 - (time.monotonic() - started)))


def battle_intervals(run_dir):
    path = run_dir / "markers.tsv"
    if not path.exists():
        return []
    intervals, start = [], None
    for line in path.read_text().splitlines():
        stamp, name = line.split("\t", 1)
        stamp = float(stamp)
        if name == "battle-start":
            start = stamp
        elif name == "battle-end" and start is not None and stamp > start:
            intervals.append((start, stamp))
            start = None
        elif name == "battle-abort":
            start = None
    return intervals


def inside(start, end, intervals):
    return intervals is None or any(a <= start <= end <= b for a, b in intervals)


def summarize_cpu(path, intervals=None):
    groups = defaultdict(list)
    if not path.exists():
        return []
    with path.open() as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            rows = groups[row["pid"]]
            if row.get("role") != "game" and retains_identity(row, rows[-1] if rows else None):
                row["role"] = "game"
                row["verification_retained"] = True
            rows.append(row)
    results = []
    for pid, rows in groups.items():
        selected = [r for r in rows if intervals is None or
                    ("unix_s" in r and inside(float(r["unix_s"]), float(r["unix_s"]), intervals))]
        if not selected:
            continue
        elapsed = cpu = 0.0
        for previous, current in zip(rows, rows[1:]):
            if "cpu_time_s" not in current:
                continue
            seconds = float(current["monotonic_s"]) - float(previous["monotonic_s"])
            used = float(current["cpu_time_s"]) - float(previous["cpu_time_s"])
            if seconds > 0 and used >= 0 and inside(float(previous["unix_s"]), float(current["unix_s"]), intervals):
                elapsed += seconds
                cpu += used
        results.append(dict(pid=pid, role=selected[-1].get("role", "unverified"),
                            verification_retained=any(r.get("verification_retained") for r in selected),
                            observations=len(selected), cpu_percent=cpu / elapsed * 100 if elapsed else None,
                            observed_cpu_percent=sum(float(r["cpu_pct"]) for r in selected) / len(selected),
                            measured_s=elapsed, max_rss_mb=max(float(r["rss_kb"]) for r in selected) / 1024))
    return results


def summarize_threads(path, intervals=None):
    groups = defaultdict(list)
    if not path.exists():
        return []
    with path.open() as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            groups[(row["pid"], row["thread_key"])].append(row)
    result = []
    for (pid, key), rows in groups.items():
        elapsed = used = 0.0
        for previous, current in zip(rows, rows[1:]):
            seconds = float(current["monotonic_s"]) - float(previous["monotonic_s"])
            cpu = float(current["cpu_time_s"]) - float(previous["cpu_time_s"])
            if seconds > 0 and cpu >= 0 and inside(float(previous["unix_s"]), float(current["unix_s"]), intervals):
                elapsed += seconds
                used += cpu
        if elapsed:
            result.append(dict(pid=pid, thread_key=key, name=rows[-1]["name"] or "unnamed",
                               cpu_percent=100 * used / elapsed, measured_s=elapsed))
    return sorted(result, key=lambda row: row["cpu_percent"], reverse=True)


HUD = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\s+\S+\s+\S+\[(\d+):[^]]+\].*metal-HUD:\s*(\d+),")


def summarize_hud(path, intervals=None, max_fps=240.0, utc_offset=None, allowed_pids=None):
    """Reject zero counters, restarts, and implausible intervals. Never join PIDs."""
    groups = {}
    if not path.exists():
        return groups
    zone = dt.datetime.strptime(utc_offset, "%z").tzinfo if utc_offset else None
    for line in path.read_text(errors="replace").splitlines():
        match = HUD.search(line)
        if not match:
            continue
        stamp, pid, count = match.groups()
        if allowed_pids is not None and pid not in allowed_pids:
            continue
        parsed = dt.datetime.fromisoformat(stamp)
        if zone:
            parsed = parsed.replace(tzinfo=zone)
        stamp, count = parsed.timestamp(), int(count)
        g = groups.setdefault(pid, dict(previous=None, accepted=[], rejected=defaultdict(int)))
        previous = g["previous"]
        g["previous"] = (stamp, count) if count else None
        if not count:
            if intervals is None or inside(stamp, stamp, intervals):
                g["rejected"]["zero counter"] += 1
            continue
        if previous is None:
            continue
        old_time, old_count = previous
        if not inside(old_time, stamp, intervals):
            continue
        seconds, frames = stamp - old_time, count - old_count
        if seconds <= 0:
            # An out-of-order record must not move the baseline backwards.
            g["previous"] = previous
            g["rejected"]["non-increasing time"] += 1
        elif frames < 0:
            g["rejected"]["counter decrease"] += 1
        elif frames / seconds > max_fps:
            g["rejected"]["above ceiling"] += 1
        else:
            g["accepted"].append((seconds, frames))
    return groups


KEY_VALUE = re.compile(r"\b(\w+)=(-?\d+(?:\.\d+)?)")


def dll_windows(path, intervals=None):
    rows = []
    if not path.exists():
        return rows
    for line in path.read_text(errors="replace").splitlines():
        if "metrics_v=1 " not in line:
            continue
        row = {k: float(v) for k, v in KEY_VALUE.findall(line)}
        required = {"unix_s", "elapsed_s", "engine_frames", "battle_completed", "counter_resets",
                    "clock_r1", "clock_r2", "world_calls", "world_total_ms", "world_max_ms",
                    "engine_time_total_ms", "engine_time_max_ms"}
        if required <= row.keys() and row["elapsed_s"] > 0 and inside(row["unix_s"] - row["elapsed_s"], row["unix_s"], intervals):
            rows.append(row)
    return rows


def detail_windows(path, intervals=None):
    result = []
    if not path.exists():
        return result
    for line in path.read_text(errors="replace").splitlines():
        if "frame_detail_v=1 " not in line:
            continue
        row = {k: float(v) for k, v in KEY_VALUE.findall(line)}
        required = {"unix_s", "elapsed_s", "frames", "sync_in_game"} | {
            f"{name}_{metric}_ms" for name in ("loop", "game", "world", "sync", "fence") for metric in ("total", "max")}
        if required <= row.keys() and row["elapsed_s"] > 0 and row["frames"] > 0 and inside(row["unix_s"] - row["elapsed_s"], row["unix_s"], intervals):
            result.append(row)
    return result


def print_summary(run_dir, section, intervals, max_fps):
    if section == "detail":
        rows = detail_windows(run_dir / "sc6vrr.log", intervals)
        frames = sum(r["frames"] for r in rows)
        if not frames:
            print("No complete frame-breakdown windows. Check whether all four detail hooks were enabled.")
            return
        print(f"Complete frame-breakdown windows: {len(rows)}; completed frame loops: {frames:.0f}")
        for key in ("loop", "game", "world", "sync", "fence"):
            total = sum(r[f"{key}_total_ms"] for r in rows)
            maximum = max(r[f"{key}_max_ms"] for r in rows)
            print(f"  {key}: summed duration / engine frame {total / frames:.3f} ms; largest call {maximum:.3f} ms")
        print("These durations overlap. World work is inside engine work, and fence work can be inside synchronization.")
        print("Do not add these numbers. A fence wait can process tasks, so its duration is not necessarily idle time.")
        if any(r["sync_in_game"] for r in rows):
            print("Synchronization occurred inside an engine tick. Those scopes overlap too.")
    elif section == "threads":
        rows = summarize_threads(run_dir / "threads.tsv", intervals)
        for row in rows[:12]:
            print(f"Process {row['pid']}, thread {row['thread_key']} ({row['name']}): {row['cpu_percent']:.1f}% CPU over {row['measured_s']:.3f} s")
        if not rows:
            print("No complete thread CPU intervals.")
        print("A thread key comes from libproc. It is not a Wine thread ID. Unnamed threads have no inferred role.")
    elif section == "cpu":
        rows = summarize_cpu(run_dir / "proc.tsv", intervals)
        if not rows:
            print("No process observations in this interval.")
        for row in rows:
            rate = row["cpu_percent"]
            print(f"Process ID {row['pid']} ({row['role']}): {row['observations']} observations; maximum resident memory {row['max_rss_mb']:.0f} MB")
            if row["verification_retained"]:
                print("  Identity was verified earlier in this run. The command and cumulative CPU time remained continuous.")
            if rate is None:
                print(f"  mean of ps observations: {row['observed_cpu_percent']:.1f}% CPU; cumulative CPU time was not recorded")
            else:
                print(f"  CPU time / elapsed time: {rate:.1f}% over {row['measured_s']:.3f} s")
        if not any(row["role"] == "game" for row in rows):
            print("No game process was verified by its native executable path. Do not combine these process IDs.")
    elif section == "hud":
        offset = None
        meta = run_dir / "meta.txt"
        if meta.exists():
            match = re.search(r"^utc_offset: ([+-]\d{4})$", meta.read_text(), re.M)
            offset = match[1] if match else None
        pid_path = run_dir / "game-pids.txt"
        allowed = set(pid_path.read_text().split()) if pid_path.exists() else None
        groups = summarize_hud(run_dir / "metal.log", intervals, max_fps, offset, allowed)
        print("Only verified game process IDs are included." if allowed is not None else
              "This old run has no verified game process IDs. Each HUD process remains separate.")
        print(f"Intervals above {max_fps:g} fps are excluded. The summary does not replace them with the limit.")
        if not offset:
            print("The old run has no UTC offset. HUD timestamps use this Mac's local time zone.")
        for pid, group in groups.items():
            values = group["accepted"]
            print(f"Process ID {pid}: accepted intervals={len(values)}; rejected={dict(group['rejected'])}")
            if values:
                elapsed = sum(s for s, _ in values)
                rates = [f / s for s, f in values]
                print(f"  total frames / total elapsed time: {sum(f for _, f in values) / elapsed:.1f} fps; min {min(rates):.1f}; max {max(rates):.1f}; measured {elapsed:.3f} s")
        if not groups:
            print("No HUD frame data.")
        print("HUD counters do not identify the scene or swapchain. Use the DLL timing for an independent comparison.")
    elif section == "dll":
        rows = dll_windows(run_dir / "sc6vrr.log", intervals)
        if not rows:
            print("No completed-work metrics. Legacy steps count scheduling decisions, not completed battle work.")
            return
        elapsed = sum(r["elapsed_s"] for r in rows)
        print(f"Windows: {len(rows)}; measured time: {elapsed:.6f} s")
        for key in ("engine_frames", "battle_completed", "clock_r1", "clock_r2"):
            value = sum(r[key] for r in rows)
            print(f"  {key}: {value:.0f}; rate {value / elapsed:.3f}/s")
        print(f"  battle counter discontinuities: {sum(r['counter_resets'] for r in rows):.0f}")
        if any(r["counter_resets"] for r in rows):
            print("  The battle rate is incomplete because counter discontinuities occurred.")
        for label, count in (("world", "world_calls"), ("engine_time", "engine_frames")):
            calls = sum(r[count] for r in rows)
            total = sum(r[f"{label}_total_ms"] for r in rows)
            maximum = max(r[f"{label}_max_ms"] for r in rows)
            print(f"  {label}: calls {calls:.0f}; mean {total / calls if calls else 0:.3f} ms; max {maximum:.3f} ms")
        print("Durations measure elapsed time, including waits. They do not measure CPU execution time.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("collect", "summary", "mark"))
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--section", choices=("cpu", "hud", "dll", "detail", "threads"), default="cpu")
    parser.add_argument("--max-hud-fps", type=float, default=240)
    parser.add_argument("--marker", choices=("battle-start", "battle-end"))
    parser.add_argument("--from-unix", type=float)
    parser.add_argument("--to-unix", type=float)
    args = parser.parse_args()
    if args.action == "collect":
        collect(args.run_dir)
    elif args.action == "mark":
        if not args.marker or not args.run_dir.is_dir():
            parser.error("Choose an existing run directory and a marker.")
        with (args.run_dir / "markers.tsv").open("a") as handle:
            handle.write(f"{time.time():.6f}\t{args.marker}\n")
        print(f"Recorded {args.marker} in {args.run_dir}")
    else:
        if args.from_unix is not None or args.to_unix is not None:
            if args.from_unix is None or args.to_unix is None or args.to_unix <= args.from_unix:
                parser.error("Choose both interval boundaries. The end must follow the start.")
            print(f"Selected UTC interval: {args.from_unix:.6f} to {args.to_unix:.6f}. Only complete measurement intervals are included.")
            print_summary(args.run_dir, args.section, [(args.from_unix, args.to_unix)], args.max_hud_fps)
            return
        print("Whole run: includes loading, menus, and battle.")
        print_summary(args.run_dir, args.section, None, args.max_hud_fps)
        intervals = battle_intervals(args.run_dir)
        if intervals:
            print("Marked battle intervals: only complete measurement intervals are included.")
            print_summary(args.run_dir, args.section, intervals, args.max_hud_fps)
        else:
            print("No complete battle interval was marked. These results do not isolate battle performance.")


if __name__ == "__main__":
    main()
