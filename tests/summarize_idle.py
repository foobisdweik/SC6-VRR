#!/usr/bin/env python3
"""Summarize the fixed final interval for the four idle tests."""

import contextlib
import datetime
import importlib.util
import json
from pathlib import Path
import sys

spec = importlib.util.spec_from_file_location("measurements", Path(__file__).with_name("measurements.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def epoch(value):
    return datetime.datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()


def summarize(run):
    window = json.loads((run / "window.json").read_text())
    end = window["end_unix"]
    start = end - 35
    bounds = [(start, end)]
    all_rows = m.dll_windows(run / "sc6vrr.log", [(window["start_unix"], end)])
    rows = m.dll_windows(run / "sc6vrr.log", bounds)
    elapsed = sum(row["elapsed_s"] for row in rows)
    active_since = None
    for row in all_rows:
        if row["battle_completed"] <= 0 or row["counter_resets"]:
            active_since = None
        elif active_since is None:
            active_since = row["unix_s"]
    checks = {
        "foreground_valid": bool(window["valid"]),
        "complete_seconds_at_least_30": elapsed >= 30,
        "battle_active_eight_seconds_before_selection": active_since is not None and active_since <= start - 8,
        "positive_battle_advances": bool(rows) and all(row["battle_completed"] > 0 for row in rows),
        "no_counter_discontinuities": all(row["counter_resets"] == 0 for row in rows),
    }
    result = dict(selection_unix=[start, end], selection_rule="Final 35 seconds of the recording",
                  checks=checks, valid=all(checks.values()), complete_windows=len(rows), elapsed_s=elapsed,
                  scene=window.get("scene"), workload_visually_verified=False,
                  presentation_user_report=window.get("presentation_user_report"),
                  idle_confirmed_by_user=window.get("idle_confirmed_by_user", False),
                  rendering_defect_user_report=window.get("rendering_defect_user_report"))
    for source, destination in (("engine_frames", "frames"), ("battle_completed", "battle_advances")):
        result[destination] = sum(row[source] for row in rows)
    result["fps"] = result["frames"] / elapsed if elapsed else None
    result["battle_rate"] = result["battle_advances"] / elapsed if elapsed else None
    detail = m.detail_windows(run / "sc6vrr.log", bounds)
    detail_frames = sum(row["frames"] for row in detail)
    result["detail_frames"] = detail_frames
    result["detail_ms_per_frame"] = {
        name: sum(row[f"{name}_total_ms"] for row in detail) / detail_frames
        for name in ("loop", "game", "world", "sync", "fence")
    } if detail_frames else {}
    meta = (run / "meta.txt").read_text()
    selected = next((line.split(": ", 1)[1] for line in meta.splitlines() if line.startswith("selected d3d11.dll: ")), None)
    loaded = (run / "loaded-files.txt").read_text() if (run / "loaded-files.txt").exists() else ""
    result["selected_graphics_dll"] = selected
    result["selected_graphics_dll_mapped"] = bool(selected and selected in loaded)
    checks["selected_graphics_dll_mapped"] = result["selected_graphics_dll_mapped"]
    env = dict(line.split("=", 1) for line in (run / "env.txt").read_text().splitlines() if "=" in line)
    expected_detail = env.get("SC6VRR_FRAME_TIMERS") == "1"
    log = (run / "sc6vrr.log").read_text()
    checks["expected_detail_timers"] = (
        "frame timers: four detail hooks created" in log and bool(detail_frames)
        if expected_detail else not m.detail_windows(run / "sc6vrr.log")
    )
    result["valid"] = all(checks.values())
    result["capture_valid"] = result["valid"]
    result["usable_for_matched_comparison"] = (
        result["capture_valid"] and result["idle_confirmed_by_user"]
        and not result["rendering_defect_user_report"]
    )
    status = run / "metal-status.json"
    result["native_exported"] = json.loads(status.read_text())["exported"] if status.exists() else False
    native = []
    for path in sorted((run / "metal-traces").glob("*.overview.json")):
        processes = json.loads(path.read_text())
        for process in processes:
            if process["PID"] != window["pid"]:
                continue
            for layer in process["Layers"]:
                timeline = [row for row in layer["Stats Timeline"]
                            if epoch(row["Start Date"]) >= start and epoch(row["End Date"]) <= end]
                seconds = sum(row["Duration MCT Seconds"] for row in timeline)
                frames = sum(row["Presented Frame Stats"]["Frame Count"] for row in timeline)
                configs = [row for row in layer["Configuration Timeline"]
                           if epoch(row["Start Date"]) < end and epoch(row["End Date"]) > start]
                drawables = [row["In-Flight Drawable Info"] for row in layer.get("On-Glass Drawable Timeline", [])
                             if epoch(row["Start Date"]) >= start and epoch(row["End Date"]) <= end]
                sizes = sorted({(row.get("MetalFX Input Width"), row.get("MetalFX Input Height"),
                                 row.get("MetalFX Target Width"), row.get("MetalFX Target Height"))
                                for row in drawables if row.get("MetalFX Enabled")})
                native.append(dict(pid=process["PID"], layer_id=layer["Layer ID"], windows=len(timeline),
                                   elapsed_s=seconds, frames=frames, fps=frames / seconds if seconds else None,
                                   layer_configurations=configs, metalfx_dimensions=sizes,
                                   shader_compilations=sum(row["Presented Frame Stats"].get("Shader Compiler", {}).get("Shader Compilations", 0) for row in timeline)))
    result["native_layers"] = native
    result["limitation"] = "GPU field semantics remain unverified. Different output paths can prevent isolating translation cost."
    (run / "idle-selection.json").write_text(json.dumps(result, indent=2) + "\n")
    with (run / "idle-selection-summary.txt").open("w") as output, contextlib.redirect_stdout(output):
        print(json.dumps(result, indent=2))
        for section in ("dll", "detail", "cpu", "threads", "hud"):
            print("\n" + section)
            m.print_summary(run, section, bounds, 240)
    print(json.dumps({key: result[key] for key in ("capture_valid", "usable_for_matched_comparison", "fps", "battle_rate", "selected_graphics_dll_mapped", "detail_ms_per_frame")}, indent=2))
    for layer in native:
        print(json.dumps({key: layer[key] for key in ("fps", "metalfx_dimensions", "shader_compilations")}))
    return result


if __name__ == "__main__":
    summarize(Path(sys.argv[1]))
