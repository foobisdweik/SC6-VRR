# Test results

Measurement correction (2026-10-15): the original CPU summaries mixed processes. The original HUD parser accepted counter restarts as valid frame intervals. The earlier values below preserve the historical summaries, so they must not establish battle performance. See [measurement changes and limitations](measurement.md). Case `10-measure-battle` now records actual battle-counter changes, clock increments, and elapsed function times. Its first live result follows the historical notes below.

Each run: cold caches (DXMT cache, including its `com.apple.metal` folder), then intro → menu → Training → one attack → quit. Run folders are in `tests/runs/` (git-ignored). The fps values come from the Metal HUD frame counter, once per second.

Valid runs (the earlier 01 runs were dry runs, cut short, or had the intro videos):

| Run | Case | Setup | Duration | PSOs compiled | Avg fps | Min fps | Intervals < 55 fps |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 20261015-150755 | 01 | MetalFX off, Retina default (on); intro videos cut, logo cards still full length | 89 s | 380 | 54.7 | 21.2 | 17 of 67 |
| 20261015-151518 | 04 | MetalFX 2x, Retina off for SC6, Game Mode forced on; boot cards and fades cut | 98 s | 380 | 55.0 | 4.0 | 16 of 75 |
| 20261015-152636 | 06 | As 04, plus DXMT native fullscreen | 120 s | 382 | 55.5 | 15.3 | 15 of 84 |

Findings:

1. **The new DXMT build works.** It has no errors and no PSO failures. The SC6 profile loads: the DXMT log shows `d3d11.preferredMaxFrameRate = 120`, `d3d11.metalSpatialUpscale = True`, and `d3d11.metalSpatialUpscaleFactor = 2.0`.
2. **MetalFX and the per-app Retina key work as planned.** The HUD (test 04, reported by the user) shows input 1728x1117 and target 3456x2234.
3. **Game Mode:** `gamepolicyctl game-mode set on` turns it on (the HUD shows Game Mode on), and the harness puts it back to automatic. The app bundle alone does not work: `gamepolicyd` saw the SC6 window with no `CFBundleIdentifier`, so the bundle's game keys are never read.
4. **Presentation is "Composited"** in both runs. So the window server composites every frame, which adds latency and probably prevents ProMotion's variable refresh. Direct presentation probably needs a native fullscreen window. See the deferred item in `notes/gamemode.md`.
5. **The cap is 60 fps,** as expected without the frame-cap DLL. The slow intervals happen during loading and menu changes. The 4 fps interval in test 04 was during the first menu load (15:15:53).
6. **About 380 PSOs compile from a cold start** before Training. This supports the PSO archive work (`notes/dxmt.md`, item 3). The DXMT log has no timestamps, so the link between compiles and slow intervals is not measured yet.
7. **Native fullscreen works (test 06).** DXMT requested it once (state 0 -> 4) and the window stayed in its fullscreen Space (4 -> 5), with no fight from Wine. Wine passed the new size to the game: on the notched 16-inch panel the content area is below the camera, so the game renders 1728x1084 and MetalFX outputs 3456x2168. The HUD still showed "Composited" in the user's screenshot, which was taken with the menu bar revealed at the top edge. The user then saw "Composited" all the time, also with the pointer away from the top.
8. **The layer format blocked direct presentation.** A DXMT dump of the window's view and layer tree (test 06 rerun, `DXMT_DUMP_LAYERS=1`) showed a correct fullscreen window with one opaque Metal layer covering it, but the layer format was 90 (`RGB10A2Unorm`, taken from SC6's R10G10B10A2 swapchain) in sRGB on a Display P3 panel. In test 07 the layer used format 94 (`BGR10A2Unorm`) and stayed sRGB, and the HUD showed "Direct" about 99% of the time, with brief "Composited" flashes. So the format was the blocker and the color space is not. The DXMT option `dxgi.metalLayerPixelFormat=bgr10a2` is now in the SC6 profile.
9. **Open: brief "Composited" flashes.** Their cause is not known. Candidates are late frames during PSO-compile hitches, and Metal HUD or system overlay updates.
10. **The frame-cap DLL works (tests 07 and 08).** The proxy `dinput8.dll` loaded through the per-app override, matched the exe hash, and read the live engine settings: `bUseFixedFrameRate=1`, `FixedFrameRate=60`, smoothing range 22–62 (unused), `t.MaxFPS=0`. This confirms the static analysis.
    - Test 07 (uncap only): 89.5 fps average (Metal HUD), up to 120.1. Every engine frame was a battle step, and the user saw the fight run too fast. This confirms that the battle simulation is frame-locked.
    - Test 08 (uncap plus gating): the original summary reported 90.8 fps average, up to 120.1. All 10 gates installed. Windows with at least 100 engine frames averaged 60.6 scheduling decisions per window (for example "frames=120 steps=60"). These values do not count completed battle calls. Window durations vary, so they are not exact rates per second. Below 60 fps, the scheduler permits only one step per rendered frame.
11. **Profiling run (test 09, run 20261015-161538) suggests three busy threads. The CPU limit remains unverified because `sample` distorts the run.**
    - Distortion: sampling a Rosetta-translated process is slow (about one snapshot every 35 ms, not 1 ms), and each snapshot pauses the game. During each 10 s window the game ran at about 3–10 fps; between windows it ran at 50–120 fps. So absolute timings from this run are not valid. The share of time per thread and per function is still useful.
    - Busy share of each thread over all 16 windows (running instead of waiting): `Thread_799984` 71% (probably the UE4 render thread), `Thread_798005` 68% (probably the UE4 game thread), `dxmt-encode-thr` 60%. All other threads are below 25%.
    - Rosetta's own work (frames in "Rosetta JIT" and "Rosetta Runtime Routines") is about 28% of the render thread's busy snapshots, 10% of the game thread's, and 27% of the DXMT encode thread's.
    - DXMT's encode thread runs mostly in unnamed frames, which are DXMT's own Windows-side code translated by Rosetta. CrossOver Preview with FEX would run DXMT as native ARM64EC code (`notes/dxmt.md`), so this thread is the one that a Preview test could speed up.
    - The profile is flat: the hottest game address takes only 1–3% of its thread. Known names among the top: `LuxMotion_SampleKeyframeTransforms` (0x1402e7780, game thread) and `_guard_check_icall` (0x1402d2bc0, the Control Flow Guard check on every indirect call). The other hot functions (`0x1415db1e0`, `0x1411adbb0`, `0x14104da70`, `0x140d2e3e0`, ...) are unnamed in Ghidra.
    - Tools: `tools/profile/analyze_sample.py` summarizes the sample files; `re/probe.sh <out> func <addr>...` names the containing function.

## Test 10: measurement without sample

Run: `tests/runs/20261015-180556-10-measure-battle`. The user entered Training and confirmed readiness. The harness ran CrossOver 26.3 with the existing DXMT build, gating enabled, a 120 fps cap, Game Mode on, and no macOS `sample`.

The original markers cover 18:07:21.924–18:07:55.870 PDT. LaunchServices reports that the game became frontmost at 18:07:24.999. Therefore, the analysis excludes the initial chat interaction and uses 18:07:24.999–18:07:55.870. The raw markers remain unchanged.

The complete DLL windows cover 30.207593 seconds within that selection. The HUD and CPU observations cover slightly different boundaries because their records arrive at different times.

| Measurement | Result |
| --- | --- |
| Engine frames | 2,329 frames, or 77.100 frames/s |
| Metal HUD | 77.114 fps across 30 accepted intervals covering 30.150 s |
| HUD range | 47.8–86.7 fps; no intervals rejected |
| Completed battle counter | 1,798 advances, or 59.521 advances/s |
| Master clock R1 | 1,798 executed increments, matching the battle counter total |
| Master clock R2 | 0 executed increments in this interval |
| Battle counter discontinuities | 0 |
| `UWorld::Tick` | 4,658 completed calls, which is two calls per engine frame |
| Mean world duration | 0.943 ms per call, or 1.885 ms summed per engine frame |
| Largest world duration | 11.863 ms |
| Mean engine frame interval | 12.970 ms |
| Mean engine timing function | 0.013 ms per engine frame, including any waits inside that function |
| Game CPU use | 233.4% over 30.103 s, derived from CPU time rather than averaged `ps` percentages |
| Launcher CPU use | 0.0% over the selected CPU intervals |

The CPU use means that the process used CPU time equivalent to about 2.33 fully occupied cores. It does not prove which threads limited rendering. The exact Training stage and characters were not recorded.

Most elapsed frame time lies outside the recorded `UWorld::Tick` calls. This narrows the next measurement to other engine work, rendering, and waits. It does not establish whether CPU work or GPU work limits performance. The timing function's small average indicates that its frame-limit wait did not dominate this interval.

The battle counter and master clock agree, so the new observations support the gating behavior independently of scheduler decisions. However, the average remains below 60 advances/s. One complete window contained 49 engine frames and 46 completed battle advances in 1.004655 s. Therefore, the existing scheduler still loses progress during slower rendering. This run does not satisfy the requirement to maintain 60 steps/s below 60 fps.

DXMT logged 428 completed pipeline compilations and no pipeline failures across the whole run. Its log records lack timestamps, so these compiles cannot be assigned to the selected battle interval. No claim about compilation causing a particular stall follows from this count.

The collector initially verified game process `91681`. Wine later changed its reported path to a temporary `SoulcaliburVI.exe` path. The CPU summary now retains the earlier verification while the command and CPU time remain continuous. A regression test covers this case. All 16 summary tests pass.

The run ended normally. Game Mode was confirmed off afterward, with its policy restored to automatic. The instrumentation's overhead remains unmeasured against an otherwise identical run without instrumentation.

Reproducible artifacts in the run directory:

- `summary.txt` contains the original marked interval and the whole run.
- `battle-foreground-summary.txt` contains the selection after the game regained the foreground.
- `battle-measurements.json` contains the structured results and the selected boundaries.

## 2026-10-16: case 11 completed on Kaer Morhen

Run: `tests/runs/20261016-143332-11-frame-breakdown`.

The helper verified five continuous seconds with game process `5622` frontmost. It then recorded from Unix `1792186434.951417` through `1792186524.970882`, or 90.019465 seconds. `window.json` records `valid: true`. The recording includes menu navigation and loading. The user reported Training on Kaer Morhen with Geralt and Groh. The camera and action pattern were not recorded.

The selected period runs from Unix `1792186485.000000` through `1792186524.970882`, or 14:34:45 through 14:35:24.971 PDT. The start is about eight seconds after the battle counter began advancing continuously. This is evidence of gameplay, but there is no independent visual boundary. Only complete measurement windows within this selection contribute to each result. Therefore, different measurements cover slightly different durations.

| Measurement | Result | Covered time |
| --- | --- | --- |
| Engine frames | 3,145 frames; 82.010 fps | 38.348867 s |
| Battle advances | 2,301 advances; 60.002/s | 38.348867 s |
| Master clock | R1: 2,301 increments; R2: 0 | 38.348867 s |
| Counter discontinuities | 0 | 38.348867 s |
| HUD | 82.0 fps; observed interval range 66.5–92.8 fps | 38.850 s |
| Native presented frames | 3,197 frames; 81.974 fps | 39.000184 s |
| Game process CPU use | 245.8% | 39.141 s |
| Busiest unnamed thread | 99.3% CPU | 39.141 s |
| `dxmt-encode-thr` | 44.7% CPU | 39.141 s |

All four detail hooks enabled in the live game. The 38 complete detail windows contain the same 3,145 frames as the baseline measurement. The mean frame-loop duration is 12.190 ms. The game engine tick takes 2.394 ms, including 1.971 ms in world updates. Frame-end synchronization takes 7.877 ms, including 7.871 ms in the render fence wait. These scopes overlap, so their durations must not all be added. The largest frame-loop call is 55.499 ms. The largest synchronization call is 51.329 ms.

Most frame time lies in the wait for rendering. A thread used nearly one full CPU core, but its name is unavailable. Therefore, its role remains unknown. A render fence wait can process tasks, so its duration does not measure pure idle time. The control comparison has not run, so measurement overhead remains unverified.

Native Metal export succeeded. It contains one process record for verified game process `5622` and one `WineMetalLayer: WineMetalView` layer. Its timestamps cover the recording. Its complete timeline windows inside the gameplay selection report these values:

- `On-GPU Walltime Stats`: mean 13.957 ms per presented frame.
- `End-to-end Walltime Stats (GPU)`: mean 3.257 ms per presented frame.
- `End-to-end Walltime Stats (CPU)`: mean 21.169 ms per presented frame.
- `Next Drawable Wait Walltime Stats`: mean 0.022 ms per presented frame.

The field names and units are preserved exactly. The reported On-GPU value exceeds the GPU end-to-end interval. Their meanings must be established before treating either as exclusive GPU occupation. `validated_gpu_metrics` remains false. No CPU-versus-GPU conclusion follows yet. Do not compare the first value directly with the 8.33 ms target as proof of a GPU limit.

The game exited at 14:36:25 PDT. Export finished after the harness's 60-second wait for persistent Wine services. Game Mode was confirmed off afterward, with its policy restored to automatic. The whole run includes counter discontinuities outside the selected recording. The selected gameplay period has none.

Artifacts include `gameplay-selection-summary.txt`, `gpu-selection.json`, `window.json`, and the native trace and overview under `metal-traces/`. The general `summary.txt` still labels the full recording as a marked battle interval. That recording includes menus, so use the separate gameplay selection for gameplay results. The next live test is case `13-timer-control` in the same scene.

The earlier launch in `20261016-143211-11-frame-breakdown` stopped before opening the game. Its registry command left Wine services running beyond the harness's 60-second wait. The harness now closes the services started by that command before clearing caches. The restarted launch succeeded. No game recording occurred in the earlier directory.

## 2026-10-16: case 13 completed; actions differed from case 11

Run: `tests/runs/20261016-143928-13-timer-control`.

The helper verified five continuous seconds frontmost, then recorded game process `7189` for 90.016555 seconds. The recording runs from Unix `1792186789.268405` through `1792186879.284960`. `window.json` records `valid: true`. The four detail timers were disabled. The DLL produced no `frame_detail_v=1` rows, as expected. The other measurement options match case 11.

The user confirmed Kaer Morhen with Geralt and Groh. The user said the actions differed from case 11. The camera was not confirmed. No independent visual boundary was captured. The selected gameplay period begins at Unix `1792186838.000000`, about nine seconds after the battle counter began advancing. It ends at the recording boundary.

The selection contains 40 complete measurement windows covering 40.238167 seconds. It contains 3,208 engine frames and 2,414 battle advances. The measured rates are 79.725 fps and 59.993 battle advances/s. R1 recorded 2,414 increments. R2 recorded none. There are no counter discontinuities in this selection. World updates take 1.944 ms per engine frame, or 0.972 ms per call.

The busiest unnamed thread used 99.3% CPU across 40.144 seconds. The named `dxmt-encode-thr` used 42.7%. The unnamed thread's role remains unknown.

Case 11 measured 82.010 fps. Case 13 is 2.786% lower. However, the actions differed, so the comparison cannot isolate timer overhead. The 5% threshold applies only after the test conditions match. It does not establish comparability when actions differ. Repeat both cases with the same specified actions before drawing an overhead conclusion. These recordings still provide measurements of their respective gameplay periods.

The game exited at 14:42:24 PDT. The remaining bottle services were closed after verifying that no Windows application remained. Native Metal export then succeeded. It contains one record for verified game process `7189` and one layer. Within the selected gameplay period, 40 complete native timeline windows contain 3,189 presented frames over 40.000143 seconds, or 79.725 fps. The HUD measured 79.8 fps over 40.146 seconds. Its accepted intervals range from 68.1 to 88.8 fps. GPU timing field meanings remain unverified. Game Mode was confirmed off, with its policy automatic.

Artifacts include `window.json`, `comparison.json`, `gameplay-selection-summary.txt`, `gpu-selection.json`, and the native trace under `metal-traces/`. `comparison.json` records `valid_for_timer_overhead: false` and `repeat_required: true` because the actions differed.

## 2026-10-16: four idle tests completed

All four recordings and native Metal exports completed. The user confirmed idle fighters in all four tests and matching composited windows in tests 3 and 4. The user reported that test 4 rendered the floor black while the grass appeared normal. The full report, measurements, paths, and limitations are in `notes/idle-comparison.md`.

| Test | Graphics | Detail timers | Engine fps | Battle advances/s | Interpretation |
| --- | --- | --- | ---: | ---: | --- |
| 1, case 14 | DXMT | Enabled | 80.077 | 59.992 | The idle selection passes the measurement checks. |
| 2, case 15 | DXMT | Disabled | 80.017 | 60.005 | The idle selection passes the measurement checks. |
| 3, case 16 | D3DMetal | Enabled | 86.180 | 60.004 | The user confirmed a composited window. |
| 4, case 17 | D3DMetal | Disabled | 86.611 | 59.995 | The black floor prevents a clean comparison of correct rendering. |

The DXMT pair differs by 0.0757%, so it shows no clear timer penalty in this idle scene. The D3DMetal pair differs by 0.4980%, but the rendering defect prevents a clean timer comparison. Native presentation and HUD counts independently agree with each engine rate. All selected native timeline windows report zero shader compilations. GPU timing field meanings remain unverified.

The output paths differ. DXMT reports MetalFX input at 1728 × 1084 and output at 3456 × 2168. Its native layer uses `BGR10A2Unorm` and its logs show native fullscreen active. D3DMetal uses a 1728 × 1117 layer in `RGB10A2Unorm`, with no reported enabled MetalFX dimensions. The user confirmed composited windows for both D3DMetal tests. These differences and the black floor prevent interpreting the higher D3DMetal fps as an isolated translation advantage.

Game Mode was confirmed off after the final run, with its policy automatic. No game or test remains running. The following paragraphs preserve the detailed record for test 1.

Run: `tests/runs/20261016-144829-14-dxmt-idle-timers`.

The user requested four tests with both fighters idle on Kaer Morhen. This first test uses DXMT with the four detail timers enabled. Existing shader caches were preserved. The scene instructions retain Geralt and Groh from the preceding runs. The user confirmed idle fighters afterward. The workload was not independently verified from video.

Game process `9648` remained frontmost for the valid 90.021221-second recording. The recording runs from Unix `1792187333.796789` through `1792187423.818010`. The final 35 seconds, from `1792187388.818010`, pass the planned checks. The battle counter began advancing more than eight seconds before that selection. Every selected window has positive battle advances and no discontinuity.

The selection contains 34 complete measurement windows covering 34.204481 seconds. It contains 2,739 engine frames and 2,052 battle advances. The rates are 80.077 fps and 59.992 battle advances/s. Native presentation measured 80.059 fps. The HUD measured 80.1 fps. Export completed successfully.

`loaded-files.txt` confirms DXMT through its mapped `d3d11.dll` and `winemetal.so` paths. `idle-selection.json` preserves the selected boundaries and checks. `idle-selection-summary.txt` preserves the final summaries. The general harness printed an older instruction to attack. The user was immediately told to follow the idle protocol. That printed instruction was corrected before test 2 launched.

## 2026-10-16: the busy rendering thread identified

The investigation used case 18 for OS thread IDs and a short Time Profiler capture. It then used case 19 to observe SC6's own thread names. The full evidence, limitations, installed DLL, and backup are in `notes/thread-identity.md`.

Case 18: `tests/runs/20261016-151102-18-thread-identity`. OS thread `143636` used 99.411% CPU before capture. The trace matched that ID, but its Windows stacks contained invalid frames. Therefore, it did not establish precise function costs or a busy-wait mechanism.

Case 19: `tests/runs/20261016-152131-19-thread-names`. The game reported `RenderThread 2` for Windows thread `688`. A read-only helper queried that thread's record. Its native thread-data value exactly matched the OS counter handle `0x7ff2067740e0`.

| Identified role | CPU use | Covered time |
| --- | ---: | ---: |
| `RenderThread 2` | 99.286% | 34.123317 s |
| Game thread | 26.749% | 34.123319 s |
| `dxmt-encode-thr` | 41.222% | 34.123318 s |

An independent query of Windows game thread `340` matched a different counter handle. The thread records passed checks of their self-pointers and Windows IDs. The result is preserved in `thread-role-proof.json`. It identifies the role through thread records rather than guessing from similar percentages.

The name observer is disabled by default and enabled by `SC6VRR_THREAD_NAMES=1`. The diagnostic DLL was installed with a backup. The build and Wine tests passed, including the check that the original exception handling continues. No DXMT source or optimization was changed. The expensive render-thread function remains unknown.

Both runs ended. Game Mode is off, its policy is automatic, and the remaining bottle services have exited.
