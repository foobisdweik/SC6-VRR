# Project decomposition — 2026-10-15

SC6 should show smoother motion at up to 120 fps while battle logic advances 60 times per second. The existing work removes the cap, but several correctness and measurement requirements remain open.

This analysis describes the source at `9f7bad4`, before the measurement changes. Step 1 has since been implemented, installed, and tested in Training. See [the current measurement status](measurement.md) and [test 10](test-results.md#test-10-measurement-without-sample).

This report applies the `recursive-decomposition` skill to the handoff and the relevant project files. Three agents examined separate batches: timing and input, measurement and launching, and DXMT with CrossOver Preview. The parent examined interpolation documents and the GPU worktree. No agent delegated further.

## Scope and evidence

- The initial search found 92 visible files across the main project and its submodules. The analysis filtered these files before reading relevant sections.
- The main project is at `9f7bad4`, which is 23 commits ahead of `origin/main`. The handoff predates its own commit.
- The GPU worktree is at `d694a44`. Its GPU report remains separate from the main project.
- The archive worktree is at `4e54e55`. The main DXMT fork is at `1029cba`.
- This was an analysis of source files, documents, stored logs, and local build prerequisites. It did not run the game.
- The analysis made no changes to source code, installed software, bottles, game files, or settings. Nothing was pushed.

The handoff's observed results remain useful: menus reached 120 fps, and the user reported normal fight speed during test 08. However, the checks below limit what the stored summaries prove.

## Findings that change the order of work

### 1. Repair measurement before comparing performance

The CPU summary averages rows from different processes. Therefore, an almost idle process lowers the reported average for the game. In the stored test 08 data, one process averages 195.1% CPU and another averages 0.3%. The summary combines them into approximately 97%. These are averages of stored observations. The data does not record process names.

The frame-rate summary joins counters without distinguishing their separate sequences. In test 09, a counter changes from 0 to 4,455 within 0.937 seconds. The formula reports 4,754.5 fps. Therefore, the reported average of 131.5 fps is unreliable. The cause of the counter discontinuity remains unknown.

Sources: [CPU summary](/Users/iphoobis/Projects/SC6-VRR/tests/summarize.sh:55), [frame-rate summary](/Users/iphoobis/Projects/SC6-VRR/tests/summarize.sh:64), and [counter discontinuity](/Users/iphoobis/Projects/SC6-VRR/tests/runs/20261015-161538-09-profile-battle/metal.log:165).

Case 09 repeatedly pauses the game while `sample` examines it. The notes report 3–10 fps during these windows. Its thread shares suggest where to investigate, but they do not establish normal performance. The proposed benefit from Preview remains a hypothesis.

Source: [recorded profiling limits](/Users/iphoobis/Projects/SC6-VRR/notes/test-results.md:27).

### 2. Complete the clock requirements before relying on the timing log

The scheduler permits at most one battle step during each rendered frame. Therefore, 45 rendered frames cannot release 60 battle steps per second. A model of the source arithmetic produced these results:

| Render rate | Scheduled steps over ten seconds | Required steps |
| --- | ---: | ---: |
| 120 fps | 600 | 600 |
| 90 fps | 600 | 600 |
| 60 fps | 600 | 600 |
| 45 fps | 450 | 600 |

This model checks arithmetic only. It does not verify the game or its hooks. A hook redirects a game function to replacement code.

Changing `if` to `while` cannot solve the complete problem. The implementation needs a path that executes every required battle system for each released step. The current log counts released steps before battle code runs. Therefore, it cannot prove that the actual battle counters advanced.

Sources: [scheduler and log](/Users/iphoobis/Projects/SC6-VRR/dll/src/sc6vrr.c:289) and [required counter tests](/Users/iphoobis/Projects/SC6-VRR/PLAN.md:86).

### 3. Make incomplete setup preserve stock timing

If a battle check fails, setup disables gating but still enables the engine hooks. Gating means skipping battle work on rendered frames that have no battle step. Therefore, uncapping can remain enabled without gating.

Some hook creation results are unchecked. Earlier changes are not consistently removed after a later failure. The DLL needs to install every required change successfully or restore every earlier change.

The DLL has no check that restores stock behavior during online play. This differs from the design in `PLAN.md`.

Sources: [setup failure path](/Users/iphoobis/Projects/SC6-VRR/dll/src/sc6vrr.c:332), [patch installation](/Users/iphoobis/Projects/SC6-VRR/dll/src/sc6vrr.c:226), and [online requirement](/Users/iphoobis/Projects/SC6-VRR/PLAN.md:52).

### 4. Preserve short input transitions

The DLL does not retain presses and releases between battle steps. The game may already preserve some transitions, but this analysis did not establish that behavior. Trace where the game polls input and where battle code consumes it before adding the merge.

Source: [input and battle gates](/Users/iphoobis/Projects/SC6-VRR/dll/src/sc6vrr.c:119).

### 5. Resolve the interpolation fields before writing to them

Interpolation creates intermediate visible states between completed battle steps. It is necessary because a higher render rate alone does not create smoother character motion.

The documents disagree about character position. `structures.md` identifies `+0xC0` as the position updated by `IntegratePhysics_PerTick`. However, `movement.md` says that function updates `+0xA0`. The plan proposes blending these fields and `+0x2090`. Their exact roles need verification in the target game.

The animation value at `lane+0x08` belongs to battle state. The documents describe transition checks that use an animation cursor. Therefore, render blending must use separate values or restore temporary changes before battle code reads them.

The camera documentation provides a candidate boundary: `UWorld::Tick` commits camera values, and the renderer reads them through `APlayerCameraManager`. This remains a documented candidate rather than fresh runtime verification.

The delay requirement needs a precise definition. Blending previous and current completed states can display an older state than the current simulation. This is an inference from the proposed algorithm. Measure the delay between input and the visible response before claiming that interpolation adds no delay.

Sources: [position fields](/Users/iphoobis/Projects/SC6-VRR/SC6-docs/docs/sc6/structures.md:200), [physics writes](/Users/iphoobis/Projects/SC6-VRR/SC6-docs/docs/sc6/movement.md:51), [animation fields](/Users/iphoobis/Projects/SC6-VRR/SC6-docs/docs/sc6/structures.md:1513), and [camera path](/Users/iphoobis/Projects/SC6-VRR/SC6-docs/docs/sc6/structures.md:809).

### 6. Preserve the archive between paired runs

A pipeline archive stores compiled GPU pipelines so that later runs can reuse them. The archive worktree's implementation is already present in the main DXMT fork. The archive files are identical between `4e54e55` and `1029cba`. Their only source difference is 17 added lines for frame statistics in `d3d11_swapchain.cpp`.

The archive and its pipeline list share the game's DXMT cache directory. The harness deletes that directory before every run. Therefore, two ordinary harness runs cannot test reuse of the archive.

The inspected installed DXMT files lack the archive's identifying strings. The handoff identifies the archive build as built but not installed. Verify which files the game loads before testing it.

Add an explicit choice between clearing and preserving caches. Record the installed build and archive files for both runs. A warm test should prove archive hits before it supports a claim about fewer compilation stalls.

The implementation starts preparation when the game creates every shader needed by a recorded pipeline. Therefore, it does not guarantee that preparation finishes before gameplay. Compare queued, finished, and failed counts with archive hits and misses.

Sources: [cache deletion](/Users/iphoobis/Projects/SC6-VRR/tests/run.sh:104), [archive paths](/Users/iphoobis/Projects/dxmt-SC6/src/d3d11/d3d11_pipeline_archive.cpp:245), and [pipeline preparation](/Users/iphoobis/Projects/dxmt-SC6/src/d3d11/d3d11_pipeline_archive.cpp:501).

### 7. Treat Preview as a separate experiment

A bottle holds the Windows environment that CrossOver uses for an application. Preview needs an `arm64` bottle to select FEX, which translates the game's x64 code. The existing `win64` bottle selects the Rosetta path described in the handoff. Creating a separate bottle preserves the current installation.

The harness accepts `CROSSOVER_APP`, but bundle launch uses a fixed copied loader. Therefore, selecting Preview can mix components from different CrossOver builds. Direct launch avoids that copied loader, but it changes the launch conditions and retains a shared Metal cache. Record those differences.

ARM64EC is the Windows architecture that lets ARM64 code interoperate with x64 code in this experiment. The local prerequisites for the custom DXMT build are incomplete. Its custom hooks and the game's patched x64 code remain unverified under FEX. Measure the combined result before predicting a speedup.

The checked toolchains lack the required ARM64EC compiler and the separate ARM64 LLVM build. The existing LLVM build targets x86_64. The configured build therefore needs additional prerequisites before compilation can begin.

Sources: [launch selection](/Users/iphoobis/Projects/SC6-VRR/tests/run.sh:80), [Preview host selection](</Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver/bin/wine:665>), and [ARM64EC build configuration](/Users/iphoobis/Projects/dxmt-SC6/build-arm64ec.txt:1).

### 8. Keep GPU estimates separate from measured improvements

The GPU report explicitly labels its savings as estimates from static analysis. It does not establish that GPU work limits Training performance. Its proposed resolution changes conflict with the handoff's recorded constraint against changing render resolution.

One candidate could preserve image quality: avoid loading old pixels when a pass will discard them. However, the estimated saving of 0.3–0.8 ms remains unverified. The proposed game hook has no located address.

Measure GPU time and the interval between frames in the same Training scene first. A 120 fps target allows approximately 8.33 ms per frame. If GPU work exceeds that budget, inspect which passes consume the time. The existing menu measurement cannot establish the battle cost.

Sources: [report scope](/Users/iphoobis/Projects/SC6-VRR/.claude/worktrees/gpu-analysis-fd2c4c/notes/gpu-analysis.md:5) and [pixel-load candidate](/Users/iphoobis/Projects/SC6-VRR/.claude/worktrees/gpu-analysis-fd2c4c/notes/gpu-analysis.md:186).

## Work packages and dependencies

| Order | Work package | Depends on | Completion check |
| --- | --- | --- | --- |
| 1 | Repair summaries and add inexpensive timers. | None. | Identify the exact game process. Separate counter sequences. Mark battle intervals. Count completed battle calls and actual clock changes. |
| 2 | Make setup restore all changes after failure. Add the online check. | None. | Each simulated setup failure leaves stock behavior. Entering online play restores stock timing. |
| 3 | Complete scheduling below 60 fps. Verify timer behavior. | Packages 1 and 2. | Actual battle counters advance 60 times per second at 120, 90, 60, and 45 fps. Pauses and stalls follow an explicit policy. |
| 4 | Preserve input between battle steps. | Packages 1–3. | Short presses register. Motion windows retain their 60 Hz duration. Replays receive one input record per battle step. |
| 5 | Verify fields and implement interpolation. | Packages 1–4. | Character and camera motion become smoother. Battle state stays unchanged. The measured input delay meets the agreed target. |
| 6 | Test the archive with paired cache conditions. | Package 1. | The second run preserves the archive, reports hits, and measures compilation stalls in the same scene. |
| 7 | Prepare and compare Preview. | Packages 1 and 2. | Verify the actual architecture, loader, graphics files, battle speed, and hooks. Compare matched battle intervals. |
| 8 | Investigate GPU candidates when measurements justify them. | Package 1. | A capture identifies expensive passes. Any change preserves the resolution constraint and shows a measured benefit. |
| 9 | Validate and package the complete result. | Packages 3–5 and selected performance changes. | Replays match with the DLL disabled. Check pacing across 24–120 Hz. The installer backs up changed files and supports restoration. |

Packages 6 and 7 test different effects. The archive may reduce compilation stalls. Preview may change CPU costs. Compare each change separately before combining them.

Two further checks belong in package 1. First, `BM_Update_Impl` is gated even though its timers use engine delta time. If each call receives only the current render interval, its timers may advance too slowly. This remains unverified. Second, the launchers restore Game Mode to automatic rather than restoring the previous policy. They need coordination if sessions overlap.

Sources: [timer assumptions](/Users/iphoobis/Projects/SC6-VRR/notes/frame-cap.md:63), [harness restoration](/Users/iphoobis/Projects/SC6-VRR/tests/run.sh:170), and [launcher restoration](/Users/iphoobis/Projects/SC6-VRR/tools/gamemode/build-app.sh:60).

Update status documents as these packages finish. `notes/dxmt.md` still describes some implemented source changes as plans. Record three separate states: implemented in source, installed locally, and verified in the game. These states currently differ.

## Verification performed for this analysis

The parent re-read the scheduler, setup failure path, summary formulas, cache deletion, and disputed structure fields. It reproduced the CPU averages, impossible frame rate, and ten-second scheduling model. Git comparisons confirmed the supplied worktree relationships. The structured findings were merged by identifier, and their cited line ranges were checked against the files.

These checks establish source behavior and defects in stored summaries. They do not establish live battle performance, FEX compatibility, pipeline reuse, interpolation correctness, or end-to-end input delay.
