# Prepared tests for the remaining frame time

## Current four-test idle comparison

All four tests and native exports completed. The user confirmed idle fighters throughout and matching composited windows for tests 3 and 4. The user reported a black floor in test 4, with normal grass. The DXMT pair shows no clear timer penalty. The D3DMetal pair cannot establish timer cost because test 4 rendered incorrectly. See `notes/idle-comparison.md` for the final results and limitations.

The user requested this comparison on 2026-10-16. Use Kaer Morhen with Geralt and Groh. Leave both fighters idle after Training loads. Keep the camera, starting positions, costumes, and graphics settings the same. Set the Training opponent to remain idle. Do not attack, move, or reset the scene during the selected period.

| Order | Case | Graphics implementation | Four detail timers |
| --- | --- | --- | --- |
| 1 | `14-dxmt-idle-timers` | DXMT | Enabled |
| 2 | `15-dxmt-idle-control` | DXMT | Disabled |
| 3 | `16-d3dmetal-idle-timers` | D3DMetal | Enabled |
| 4 | `17-d3dmetal-idle-control` | D3DMetal | Disabled |

Each case uses the same installed DLL, baseline measurements, thread CPU counters, and basic native Metal tracing. Encoder timing and macOS `sample` remain disabled. The harness selects the graphics implementation through the launch environment, so it does not change the bottle's saved selection. It records the requested implementation and its DLL hash. `loaded-files.txt` preserves mapped files from the verified game process. Check this evidence before claiming that the requested implementation loaded. `libd3dshared` alone is insufficient because CrossOver uses it with both implementations.

Each recording starts after five continuous seconds frontmost and lasts 90 seconds. Menus and loading can occur inside the recording. Select the final 35 seconds for all four cases. Require at least 30 seconds of complete measurement windows. The battle counter must have advanced continuously for at least eight seconds before that selection begins. Require positive battle advances and no counter discontinuities throughout the selected period. Confirm that both fighters stayed idle. If these conditions fail, repeat that case instead of choosing a more favorable interval.

Preserve existing shader caches for all four cases. These are measurements of settled gameplay, not cold-start compilation tests. Inspect the native timeline for compilation during the selected period. If compilation occurs, flag the affected case before interpreting small differences. The caches cannot be shared between graphics implementations, so their contents are not identical.

Keep the user's resolution settings unchanged. The installed DXMT profile enables MetalFX upscaling, native fullscreen, and a BGR10A2 layer. D3DMetal may use a different output path. Record the actual render and output dimensions, MetalFX state, layer format, and presentation behavior where available. If those differ, report the cross-implementation result as a comparison of the existing configurations. Do not attribute that difference solely to translation cost.

First compare timers enabled versus disabled within each graphics implementation. The 5% repeat threshold applies only when the scene and actions match. Then compare the two cases with timers disabled to assess the existing configurations. These results apply to idle gameplay. They do not establish timer overhead or performance during attacks and effects.

The earlier protocol below describes cases 11–13 and remains available for reference.

The first Training measurement found about 1.89 ms inside world updates during a frame interval of 12.97 ms. These tests will measure the other work and waits. The target for 120 fps is 8.33 ms per frame.

Cases 11 and 13 completed on 2026-10-16. Both native exports succeeded. Case 11 measured about 82 fps. Case 13 measured about 79.7 fps. Both measured about 60 battle advances per second. The user confirmed that the actions differed, so this pair cannot isolate timer overhead. The later cases 14–17 repeat the comparison with idle fighters. The meanings of the GPU timing fields still need clarification. See `notes/test-results.md` for the measurements and limitations.

## Test cases

| Case | Purpose | Change relative to case 11 |
| --- | --- | --- |
| `11-frame-breakdown` | Measure the full frame loop, engine tick, world updates, synchronization, thread CPU time, and basic GPU data. | This is the main test. |
| `13-timer-control` | Check whether the four new CPU timers affect performance. | The four new hooks are disabled. All other measurement options remain the same. |
| `12-gpu-detail` | Inspect which GPU work takes time when the main test points toward the GPU. | Timing for individual Metal command encoders is enabled. |

A Metal command encoder groups commands for one kind of GPU work. The optional GPU test adds timing for those groups. That timing can add overhead, so compare it with case 11 rather than treating its fps as an unchanged baseline.

Run case 11 first. Then use case 13 to check overhead in the same scene with the same specified actions. If the scene or actions differ, repeat both cases before interpreting the difference as timer overhead. For a pair with matching conditions, repeat if fps differs by more than 5%. This threshold triggers another check. It does not prove which measurement caused the difference. Run case 12 only if the basic GPU data needs more detail.

## Conditions to hold constant

- Use CrossOver 26.3 and the existing DXMT installation for all three cases.
- Use the same Training stage, characters, camera, graphics settings, and action pattern.
- Keep render resolution and MetalFX settings unchanged.
- Keep Game Mode on, Retina mode off, the 120 fps cap, and battle gating enabled.
- Keep macOS `sample` off. None of these cases requests stack samples.
- Allow menu navigation and loading within the recording. Separate these periods from gameplay during analysis.
- Each harness launch still clears the DXMT cache, so this is not an archive-reuse test.

Record the exact stage and characters with `--scene` when arming the interval. Do not infer them from the previous run because its scene names were not recorded.

## What the new timers measure

The DLL enables these hooks only when `SC6VRR_FRAME_TIMERS=1` and baseline measurement is available. Each target has a fixed address for the supported executable:

| Timer | Address | Calling convention checked from disassembly |
| --- | --- | --- |
| Frame loop | `0x140396450` | The loop object is passed in `RCX`. |
| Game engine tick | `0x141e38f70` | The engine is in `RCX`, delta time is in `XMM1`, and the idle flag is in `R8B`. |
| Frame-end synchronization | `0x1421876d0` | The object is in `RCX`, and the option flag is in `DL`. |
| Render fence wait | `0x1415efec0` | The object is in `RCX`, and the task-processing flag is in `DL`. |

A calling convention specifies where a caller places the function's arguments. The Wine test checks that the wrappers forward these arguments unchanged.

The frame-loop address was found by following the direct call to `UpdateTimeAndHandleMaxTickRate`. Its call to engine vtable offset `+0x268` selects the game engine tick. The tick contains a source-file reference to `GameEngine.cpp`. The synchronization roles follow the frame-loop call order and the helpers' task and event operations. These are roles inferred from disassembly rather than recovered debug symbols.

The DLL checks the executable hash and each function's first bytes before creating these hooks. If one hook cannot be created, it removes the detail hooks already created. The existing baseline measurements remain separate.

The detail log uses `frame_detail_v=1`. It records aggregate durations after completed frame loops. Recursive calls of the same measured scope are counted only at their outer boundary.

Some durations overlap because one measured function calls another. World work is inside the engine tick. A render fence wait can be inside frame-end synchronization. Therefore, do not add their durations. A fence wait can process tasks, so its elapsed time does not necessarily mean that the thread was idle.

## Thread CPU measurements

The collector reads cumulative CPU counters through Darwin's `proc_pidinfo` API about once per second. It does not request stack samples or suspend the process. The counters report CPU execution time, while the DLL timers report elapsed time that can include waits.

The collector preserves each thread's reported name. It does not label unnamed threads as render or game threads. A thread key from this API is not a Wine thread ID.

The native test checked the structure layout and compared counter changes with Python's own thread CPU clock. The units and counter changes matched on this Mac. The total overhead in SC6 still needs the control comparison.

Source: [Apple's kernel implementation of the thread counters](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/bsd_kern.c).

## Native GPU data

These cases set `METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1`. This enables Apple's markers for individual presented frames. After the game exits, the harness uses `/usr/bin/metalperftrace` to export data for the completed recording interval. The overview filters for verified game processes.

Case 11 uses basic GPU timing. Case 12 adds `MTL_HUD_ENCODER_TIMING_ENABLED=1`. The old HUD console log remains an independent cross-check, but it does not replace the native export.

The export preserves these files:

- `metal-status.json` records whether native export commands succeeded.
- `metal-collect.json` and `metal-capture.log` preserve the collection result.
- `metal-traces/*.atrc` preserves the native traces.
- `metal-traces/*.overview.json` preserves the game-filtered output, including a timeline when the tool provides one.

Successful export does not prove that GPU timing fields exist. `validated_gpu_metrics` stays false until the first live output is inspected. Confirm the game process, active Metal layer, units, timestamps, and timing fields before calculating GPU results. If the export fails, report GPU time as unavailable. Do not substitute CPU elapsed time for it.

The installed references are `/usr/share/man/man1/metalperftrace.1` and `/usr/share/man/man1/MetalPerformanceHUD.1`. Apple recommends native performance data in place of the older console logging. See [Apple's HUD guide](https://developer.apple.com/documentation/xcode/monitoring-your-metal-apps-graphics-performance).

## Recording procedure

1. Start `tests/run.sh 11-frame-breakdown` after the user says to begin.
2. Record the planned Training stage, characters, camera, and action pattern.
3. Arm the interval with `tests/record_window.py arm`, using the explicit run directory.
4. Return to the game. The helper requires five continuous seconds in the foreground before marking the start.
5. Navigate to Training, let loading finish, and follow the chosen action pattern.
6. Keep the game in the foreground throughout the 90-second recording, including menu navigation and loading.
7. Wait until the recording ends before returning to chat.
8. Quit the game so that the harness exports the native Metal data and writes its summary.

The helper's existing `battle-start` and `battle-end` markers now bound the recording, which can include menus and loading. They do not prove when gameplay began. Select confirmed gameplay periods separately before reporting gameplay results. If the gameplay boundary cannot be confirmed, label the result as a mixed recording.

The helper polls foreground state twice per second. If the game loses the foreground or its verified process changes, the helper aborts the interval. The summary excludes aborted intervals. These checks cannot rule out a focus change shorter than the polling interval.

Example command after launch:

```sh
python3 -I tests/record_window.py arm tests/runs/<run-name> \
  --seconds 90 --settle 5 \
  --scene 'Stage: <name>; characters: <names>; camera: default; actions: <fixed pattern>'
```

## Acceptance checks

- All four detail hooks must be enabled before interpreting the frame breakdown.
- The completed frame-loop count and the independent engine-frame count must be consistent within their different reporting boundaries.
- The battle-counter and master-clock totals must be compared independently of scheduler decisions.
- At least 30 seconds of complete measurement windows must remain within a confirmed gameplay period inside the valid foreground interval.
- CPU observations must keep the launcher separate from the verified game process.
- GPU data must match the game process and active layer before it supports a conclusion.
- Unknown GPU fields, missing data, export failures, counter resets, and scene changes must remain visible in the report.

## How to interpret the results

If the game engine tick takes most of the frame, inspect work within that tick before and after world updates. If synchronization takes most of the frame, compare its timing with GPU work and each thread's CPU execution time.

A large wait with little CPU use does not by itself prove a GPU limit. Another CPU thread might still be preparing commands. If GPU work is long, inspect its contribution against the 8.33 ms target. If GPU work is short but a named DXMT thread uses most of a core, the CPU side of graphics translation becomes a stronger candidate.

Use Apple's frame-level GPU interval rather than adding potentially overlapping command durations. Keep all conclusions provisional until the timer-control comparison checks the measurement overhead.
