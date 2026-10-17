# Four idle tests on Kaer Morhen, 2026-10-16

Later follow-up: the reproduced near-one-core thread was identified as SC6's `RenderThread 2`. The match used the game's thread name, its Windows thread record, and the exact OS counter handle. See `thread-identity.md`. The original measurements below remain unchanged. IDs from later processes must not be assigned retroactively to these captures.

All four recordings and native Metal exports completed. The DXMT pair shows no clear cost from the four detail timers in idle gameplay. The D3DMetal pair cannot establish that cost because the user reported incorrect rendering in test 4.

## Conditions

The user confirmed that both fighters stayed idle in all four tests. The scene used Kaer Morhen with Geralt and Groh. The user confirmed that tests 3 and 4 used the same composited window. In this mode, macOS combines the game image with the desktop before display.

Each recording began after five continuous seconds with the verified game process frontmost. Each recording lasted 90 seconds. Menu navigation and loading remained inside those recordings. The analysis uses the final 35 seconds of each recording. Only complete measurement windows contribute, so their summed durations differ slightly.

Every selection passes these checks:

- At least 30 seconds of complete measurement windows remain.
- The battle counter advanced continuously for at least eight seconds before the selection began.
- Every selected window contains positive battle advances and no counter discontinuity.
- Mapped files confirm the requested graphics implementation.
- The four detail timers produced data when enabled and no data when disabled.

The installed DLL, game executable, CrossOver 26.3, Game Mode, Retina setting, 120 fps cap, and battle gating stayed the same. Existing shader caches were preserved. No macOS `sample` capture ran. Encoder timing stayed disabled. Native timelines report zero shader compilations in the complete selected windows. This does not prove that no compilation occurred outside those windows.

The idle workload was confirmed by the user. It was not independently verified from video. No conclusion about timer cost during attacks or effects follows from these idle tests.

## Results

| Test | Case | Graphics | Detail timers | Engine fps | Native presented fps | Battle advances/s |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | `14-dxmt-idle-timers` | DXMT | Enabled | 80.077 | 80.059 | 59.992 |
| 2 | `15-dxmt-idle-control` | DXMT | Disabled | 80.017 | 80.058 | 60.005 |
| 3 | `16-d3dmetal-idle-timers` | D3DMetal | Enabled | 86.180 | 86.147 | 60.004 |
| 4 | `17-d3dmetal-idle-control` | D3DMetal | Disabled | 86.611 | 86.617 | 59.995 |

**Test 4 rendered the floor black.** The user reported that the grass looked normal. Preserve its measured frame rate, but exclude it from conclusions that require matching, correct rendering.

| Test | Complete DLL windows | Measured seconds | Engine frames | Battle advances |
| --- | ---: | ---: | ---: | ---: |
| 1 | 34 | 34.204481 | 2,739 | 2,052 |
| 2 | 34 | 34.230368 | 2,739 | 2,054 |
| 3 | 33 | 33.198024 | 2,861 | 1,992 |
| 4 | 34 | 34.152638 | 2,958 | 2,049 |

The HUD independently measured 80.1, 80.0, 86.1, and 86.6 fps. The selected HUD intervals contain no rejected counters. Each method has its own reporting boundaries, so its totals need not match exactly.

## What the comparison supports

Under DXMT, enabling the four timers changed the measured fps by +0.0757% relative to disabling them. This pair shows no clear timer penalty. One pair does not establish an exact cost or a confidence interval.

Under D3DMetal, enabling the timers changed the measured fps by −0.4980% relative to disabling them. However, test 4's rendering defect prevents a clean interpretation. The numeric difference being below 5% does not remove that defect. Do not conclude that the timers caused, prevented, or exposed the black floor.

The D3DMetal control recorded 8.241% more fps than the DXMT control. That is a measurement of these configurations, not evidence of an isolated translation advantage. Test 4 rendered incorrectly, and the output paths differ between graphics implementations.

## Output paths differed

| Graphics | Native layer dimensions | Native pixel format | Reported MetalFX dimensions | Presentation evidence |
| --- | --- | --- | --- | --- |
| DXMT, tests 1–2 | 3456 × 2168 | `BGR10A2Unorm` | 1728 × 1084 input, 3456 × 2168 output | DXMT logs show native fullscreen active. The installed profile requests this mode. |
| D3DMetal, tests 3–4 | 1728 × 1117 | `RGB10A2Unorm` | No enabled MetalFX dimensions were reported. | The user confirmed a composited window for both tests. |

These are native layer and MetalFX measurements. They do not independently establish every internal rendering resolution used by the game. The user's graphics settings were left unchanged. Different layer dimensions, upscaling, pixel formats, and presentation can change the work performed. Therefore, the cross-implementation fps difference cannot be attributed solely to translation cost.

## CPU measurements

| Test | Game process CPU use | Busiest thread CPU use | Named DXMT encoding thread CPU use |
| --- | ---: | ---: | ---: |
| 1 | 228.2% | 99.3%, unnamed | 42.0% |
| 2 | 228.4% | 99.4%, unnamed | 42.1% |
| 3 | 175.6% | 66.4%, unnamed | Not present under that name |
| 4 | 177.1% | 67.3%, unnamed | Not present under that name |

100% CPU corresponds to one fully occupied CPU core over the measured interval. Thread observations cover about 33–34 seconds within each selection. The unnamed threads have not been assigned roles. The lower CPU use under D3DMetal applies to its observed configuration and does not isolate the effect of translation.

In test 1, the full frame loop averages 12.484 ms per frame. The game engine tick takes 1.928 ms, including 1.574 ms in world updates. Synchronization takes 9.008 ms, including a 9.003 ms render fence wait. The render fence wait can process tasks, so its elapsed time does not prove that the CPU was idle.

In test 3, the corresponding means are 11.601 ms for the frame loop, 1.768 ms for the engine tick, and 1.439 ms for world updates. Synchronization takes 8.439 ms, including 8.434 ms in the render fence wait. These durations overlap. Do not add all of them.

## Rendering defect and remaining uncertainty

Both D3DMetal Wine logs contain `Unsupported: D3D11 timestamp query`. Both contain the same GStreamer assertion from the introduction videos. Neither warning identifies the cause of the black floor in test 4. The native timeline reports no shader compilations in that selection. That does not rule out incorrect shaders or other rendering errors.

The native exports preserve GPU timing fields, but their meanings remain unverified. `validated_gpu_metrics` remains false. Native presentation counts provide an independent fps comparison. They do not establish exclusive GPU occupation or distinguish a CPU limit from a GPU limit.

The next investigation should identify the unnamed DXMT thread that uses nearly one full core. The D3DMetal black floor needs a separate check before treating its performance as correct rendering. No additional live run has started.

## Artifacts and reproduction

The run directories under `tests/runs/` are:

- `20261016-144829-14-dxmt-idle-timers`
- `20261016-145201-15-dxmt-idle-control`
- `20261016-145519-16-d3dmetal-idle-timers`
- `20261016-145834-17-d3dmetal-idle-control`

Each directory preserves `window.json`, `idle-selection.json`, `idle-selection-summary.txt`, `loaded-files.txt`, raw CPU observations, DLL logs, and native traces. `window.json` records the user's confirmation. Test 4 records the black floor. Its `idle-selection.json` reports `capture_valid: true` but `usable_for_matched_comparison: false`.

Regenerate a fixed selection after export with:

```sh
python3 -I tests/summarize_idle.py tests/runs/<run-name>
```

The capture checks validate foreground state, counters, durations, mapped graphics files, and timer state. They do not validate pixels. The separate comparison flag excludes a user-reported rendering defect.

Game Mode was confirmed off after the fourth run. Its policy was restored to automatic. All game processes and the remaining bottle services have exited.
