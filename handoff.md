# SC6-VRR handoff (2026-10-16)

## Active direction

The user deferred Metal rendering, HDR, ProMotion, interpolation, and game timing changes.
Focus on native ARM64 battle logic while preserving the original behavior and timing.
The user requested broad tests, rather than more isolated function probes.
Use connected battle functions and recorded sequences that exercise both fighters.
See [the native plan](notes/native-port.md) and [the broader test plan](notes/native-battle-tests.md).

The latest proof matches all observed outputs from 64 actual movement calls in `SC6-fex`.
Eleven calls changed position. No airborne state was captured.
The temporary recorder was removed after SC6 exited, and the existing backup was preserved.
Recovered code and captures remain outside Git under `/Users/iphoobis/Library/Application Support/SC6-VRR/native-investigation/`.
This proof is preliminary and does not satisfy the broader test requirement.

## Earlier VRR investigation

The reports and priorities below describe the earlier work. The active direction above takes precedence.

Thread identification completed: the reproduced full-core thread is SC6's `RenderThread 2`. In case 19, its Windows thread ID `688` mapped through its thread record to OS counter handle `0x7ff2067740e0`, which measured 99.286% CPU. The independent game-thread check mapped Windows ID `340` to `0x7ff2061fa0e0` at 26.749%. DXMT's named encoding thread used 41.222%. This identifies the role, not the expensive function. The proof is in `tests/runs/20261016-152131-19-thread-names/thread-role-proof.json`, and the full account is `notes/thread-identity.md`. The earlier case 18 profiler matched the busy OS thread exactly but produced unreliable Windows stacks. Do not infer precise function costs or a busy-wait mechanism from them.

The installed diagnostic DLL has SHA-256 `3f221a93f84ac6435236b12de4e18c5b013aa81b4e2794e912624a8e230cf31e`. Its optional name observer is enabled only by `SC6VRR_THREAD_NAMES=1`. The prior DLL is backed up at `/Users/iphoobis/Library/Application Support/SC6-VRR/backups/thread-names-dll-20261016-152115/dinput8.dll`. All 27 Python tests, the DLL build, and the Wine tests passed. The exception observer preserves original exception handling. It logs names requested for other threads but does not pass those names to macOS. The final identification therefore uses read-only Windows thread records. Game Mode is off, its policy is automatic, and all game processes and bottle services have exited.

Profiler output handling: an initial preflight exposed a Hugging Face token in tool output through an unfiltered trace index. The user was notified and advised to rotate it. The scratch trace was deleted. Later launches use an environment allowlist, and the live trace was checked to contain zero credential-named environment fields. Never print a raw `xctrace export --toc` result. Export selected data tables or remove the environment section before inspecting the index.

Four-test result on 2026-10-16: all four idle recordings and native exports completed. The user confirmed idle fighters in all four tests and the same composited window in tests 3 and 4. Cases 14–17 measured 80.077, 80.017, 86.180, and 86.611 fps. Each measured about 60 battle advances/s. The DXMT pair differs by 0.0757%, so it shows no clear timer penalty in this idle scene. Test 4 rendered the floor black while the grass appeared normal. Its capture remains valid, but it is excluded from conclusions that require matching, correct rendering. The D3DMetal pair cannot establish timer cost. Cross-implementation comparisons also differ in upscaling, native layer dimensions, pixel format, and presentation. Do not infer an isolated translation advantage from the higher D3DMetal fps. The full report is `notes/idle-comparison.md`. The next investigation should identify the render thread's expensive work and separately check D3DMetal's black floor.

Completed request: run four idle tests on Kaer Morhen with Geralt and Groh. Cases 14 and 15 use DXMT with the four detail timers enabled then disabled. Cases 16 and 17 use D3DMetal with the same timer change. The harness supports an explicit graphics selection and records its DLL hash and mapped files. These four cases preserved shader caches and graphics settings. The fixed final 35-second selections all retain at least 30 seconds of complete measurement windows. Use `tests/summarize_idle.py` to regenerate the summaries. The procedure is in `notes/frame-tests.md`.

Case 13 update on 2026-10-16: `tests/runs/20261016-143928-13-timer-control` completed and exported native Metal data. The user confirmed Kaer Morhen, Geralt, and Groh, but said the actions differed from case 11. Therefore, the pair cannot isolate timer overhead. The 5% threshold does not establish comparability when actions differ. Repeat both cases with the same specified actions before drawing an overhead conclusion. The four detail timers were disabled, and no detail rows were produced. The gameplay selection measured 79.725 fps and 59.993 battle advances/s. Native presentation measured 79.725 fps. The HUD measured 79.8 fps. Game Mode is off and its policy is automatic. See `notes/test-results.md` and the run's `comparison.json`.

Continuation on 2026-10-16: case `11-frame-breakdown` completed in `tests/runs/20261016-143332-11-frame-breakdown`. The user reported Kaer Morhen, Geralt, and Groh. Its recording lasted 90.019465 seconds after five continuous seconds frontmost. The gameplay selection, inferred from continuous battle-counter advances, measured 82.010 engine frames/s and 60.002 battle advances/s. The HUD and native Metal timeline both measured about 82 fps. All four detail hooks enabled. The rendering wait averaged 7.877 ms within a 12.190 ms frame. One unnamed thread used 99.3% CPU. The native export succeeded, but its GPU timing fields need clarification before attributing the limit. Case 13 must still check timer overhead. Game Mode is off and its policy is automatic. See `notes/test-results.md` for boundaries and limitations. An earlier launch stopped before opening the game because the registry command left Wine services running. The harness now closes those services before clearing caches.

This file is for whoever continues the project. It says what works, what is installed on this Mac, what is open, and the research for the next task: testing SoulCalibur VI in CrossOver Preview with FEX and an ARM64EC build of DXMT.

Read with it: `PLAN.md` (the plan and step status), `notes/test-results.md` (every measured run), `notes/frame-cap.md`, `notes/dxmt.md`, `notes/gamemode.md`.

Continuation on 2026-10-15: step 1's measurement changes are implemented and installed. The live test completed at 18:09 PDT. Its foreground Training selection measured 77.1 engine frames/s and 59.52 completed battle advances/s. The battle counter total matched master-clock increments. Most elapsed frame time lies outside `UWorld::Tick`. See `notes/test-results.md` and `notes/measurement.md`. The corrected summaries separate CPU processes and reject invalid HUD counters. Earlier `steps=` logs count scheduler decisions rather than completed battle work.

Prepared at 18:34 PDT: cases `11-frame-breakdown`, `12-gpu-detail`, and `13-timer-control`. The extended DLL is installed with a backup. Its isolated tests and preflight pass. `notes/frame-tests.md` contains the protocol. `tests/record_window.py arm` waits for five continuous seconds with the game frontmost, then records for 90 seconds. Menu navigation and loading can occur within that interval. Gameplay must be selected separately during analysis. New thread counters and native Metal export are wired into the harness. The user asked to stand by before starting logging. No case 11–13 game run or Metal collection has started.

## 1. Goal and current result

Goal: SoulCalibur VI (SC6) renders at up to 120 fps with ProMotion pacing on the MacBook Pro 16 (M1 Pro, macOS 27.2), under CrossOver 26.3 with DXMT, while the battle logic keeps its 60 Hz timing.

Current result (measured, see `notes/test-results.md`):

- The 60 fps cap is removed. Menus render at up to 120 fps.
- The battle simulation holds 60 steps per second at higher frame rates. The user confirmed normal fight speed and normal input (test 08).
- In the Training fight the engine reaches only 65–86 fps. Profiling (test 09) shows three CPU threads at 60–71% busy: the UE4 render thread, the UE4 game thread, and DXMT's encode thread. Rosetta's own work is 10–28% of those threads' busy time.
- Character motion still updates at 60 Hz, because interpolation (plan step 7) is not done. So motion looks the same as before even when the frame rate is higher.
- Presentation is "Direct" (no window-server compositing) about 99% of the time, with native fullscreen, a BGR10A2 Metal layer, and MetalFX 2x (1728x1084 → 3456x2168).
- Game Mode turns on through Xcode's `gamepolicyctl`.

## 2. Repositories

| Repo | Path | Branch | State |
| --- | --- | --- | --- |
| SC6-VRR | `/Users/iphoobis/Projects/SC6-VRR` | `main` at `38dba1d` | 22 commits ahead of `origin/main` (`foobisdweik/SC6-VRR`), **not pushed**. The user asked not to push yet. |
| DXMT fork | `/Users/iphoobis/Projects/dxmt-SC6` | `sc6/native-fullscreen` at `1029cba` | Local only, not pushed. Upstream `AGENTS.md` bans AI-generated contributions, so none of this goes upstream. |

Other branches:

- `SC6-VRR`: `claude/gpu-analysis-fd2c4c` (commit `d694a44`) holds `notes/gpu-analysis.md`, a static GPU analysis from another session. It is **not merged**. Its top GPU candidates: avoid wasted tile loads in DXMT, half-resolution effects, lighter SSAO and bloom. The user rejected render-resolution changes.
- `dxmt-SC6`: `claude/sc6-pso-archive-e33d13` is already merged into `sc6/native-fullscreen` (merge `1029cba`). `sc6/present-options` is an older branch, superseded.

## 3. What is installed on this Mac now

| Item | Location | How to undo |
| --- | --- | --- |
| DXMT build (from `9c5d59e`: SC6 profile, native fullscreen, BGR10A2 layer, frame-stats logging; **without** the PSO archive) | `/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/dxmt/x86_64-windows` and `x86_64-unix` | Restore from `~/Library/Application Support/SC6-VRR/backups/crossover-26.3-dxmt-20261015-135807/dxmt` |
| PSO-archive build (merge `1029cba`) | Built in `dxmt-SC6/build/` only. **Not installed**, not copied to `install/`. | — |
| sc6vrr frame-cap DLL | `.../SoulcaliburVI/Binaries/Win64/dinput8.dll`, plus per-app override `HKCU\Software\Wine\AppDefaults\SoulcaliburVI.exe\DllOverrides` `dinput8=native,builtin` in the `Steam` bottle | Delete the DLL and the key. Without `SC6VRR_*` variables the DLL only logs (to `Binaries/Win64/sc6vrr.log`). |
| Boot-skip pak (logo and caution cards 0 s, fades one frame) | `.../Content/Paks/~mods/pakchunk999-WindowsNoEditor_P.pak` | Delete the `~mods` folder |
| Intro videos replaced by 0.5 s black clips (`ST_001.mp4`, `SC_20th_overseas_1080p.mp4`) | `.../Content/Movies/` | Copy back from `~/Library/Application Support/SC6-VRR/backups/movies-original` |
| Per-app Retina mode off | `HKCU\Software\Wine\AppDefaults\SoulcaliburVI.exe\Mac Driver` `RetinaMode=n` (set by the last test) | `tests/run.sh` sets or removes it per case |
| Game app bundle | `~/Applications/SC6-VRR/SoulCalibur VI.app` (built by `tools/gamemode/build-app.sh`) | Delete it |

Not installed: `config/Engine.SystemSettings.ini` (post-effect overrides). The game's `Engine.ini` has no `[SystemSettings]` section.

The SC6 AES key (needed to read the paks) was given by the user. It is stored only in this session's scratchpad (`.../scratchpad/sc6.aeskey`), which disappears with the session. Never commit it.

## 4. How to test

- `tests/run.sh --list` lists the cases. `tests/run.sh <case>` runs one with background logging. The user plays: intro, menu, Training, an attack, quit. The summary is written when the game exits (`tests/runs/<time>-<case>/summary.txt`, git-ignored).
- Each run clears SC6's DXMT cache (`$(getconf DARWIN_USER_CACHE_DIR)dxmt/SoulcaliburVI.exe`, which also holds DXMT's redirected Metal cache).
- Useful cases: `06-native-fullscreen` (full DXMT profile), `08-fixed-step` (full profile plus cap removal and 60 Hz gating), `09-profile-battle` (as 08, plus CPU sampling and DXMT frame stats).
- Run tests with the Bash tool in the background and poll the run folder. The harness refuses to start while a `wineserver` is running.

Known limits:

- `sample` on the Rosetta-translated game is slow (about one snapshot every 35 ms) and pauses the game, which drops it to 3–10 fps during each 10 s window. Use the thread shares, not the absolute times. `tools/profile/analyze_sample.py` summarizes the files.
- A separate Game Mode monitor exists on this Mac (`~/Library/Application Support/CrossOverGameMode/monitor.log`). It also sets Game Mode on and back to automatic when it sees SC6 or SF6. It may overlap with the harness's and the launcher's `gamepolicyctl` calls.

## 5. Open work (by priority)

1. **Find the CPU bottleneck without distortion.** The DLL now times `UWorld::Tick` and the engine's timing function. Test 10 verified these measurements in Training without `sample`. The world calls average 1.885 ms summed per engine frame, compared with a 12.970 ms frame interval. The next measurement should cover other engine work, rendering, and waits. This run does not distinguish a CPU limit from a GPU limit. See `notes/measurement.md` and `notes/test-results.md`.
2. **Test CrossOver Preview with FEX** (section 6). DXMT's encode thread would then run as native ARM64 code instead of translated x86 code.
3. **Install and test the PSO archive** (`dxmt-SC6` merge `1029cba`). Run the game twice; the second run should log `PipelineArchive` hits and early compiles. Copy all four files together (`d3d11.dll`, `dxgi.dll`, `winemetal.dll`, `winemetal.so`), because the build adds unix calls 147 and 148.
4. **Interpolation** (plan step 7): blend character render position (`chara+0x2090`), animation frame (`lane+0x08`), and the camera POV (`APlayerCameraManager+0x410..+0x428`) between 60 Hz steps. Research has started; see `SC6-docs/docs/sc6/structures.md` (camera POV section) and `movement.md`.
5. **Input merge** (plan step 5): poll input every rendered frame and merge it into the next 60 Hz step.
6. GPU candidates from `notes/gpu-analysis.md` (branch above), when the battle turns out to be GPU-bound.

## 6. Research: CrossOver Preview with FEX and ARM64EC DXMT

Read-only research done on 2026-10-15. Nothing was created or changed.

### 6.1 CrossOver Preview facts

- Installed: `/Applications/CrossOver Preview.app`, version 27.0.0.41069 (build 20261006). Bottles live in the same folder as CrossOver's: `~/Library/Application Support/CrossOver/Bottles`. Only `Steam` exists (`WineArch = win64`, no `Preview` key).
- The Preview `wine` script (`.../CrossOver Preview.app/Contents/SharedSupport/CrossOver/bin/wine`) exits unless the bottle's `cxbottle.conf` has `[Bottle] "Preview" = "1"`. So CrossOver 26.3 bottles cannot run in Preview as they are.
- Host choice (script lines 665–679): it uses ARM64 Wine (`lib/wine/aarch64-unix`) unless the bottle is `win64` **and** `lib/x86_64` exists. Preview has `lib/x86_64`, so a `win64` bottle runs x86_64 Wine under **Rosetta**, not FEX.
- FEX needs a bottle with `WineArch = arm64`. In such a bottle, x64 programs run through FEX inside ARM64 Wine (`lib/wine/aarch64-unix/libarm64ecfex.so`, `libwow64fex.so`, `aarch64-windows/xtajit64.dll`). FEX caches go to `<bottle>/fex-emu/`, and the script sets `FEX_X87REDUCEDPRECISION=1`.
- Create one with: `"/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver/bin/cxbottle" --bottle SC6-FEX --create --template win10_64 --winearch arm64`. The template sets `Preview=1`. `cxbottle --copy SOURCE` also exists, but a copy of `Steam` stays `win64` (Rosetta), and changing `WineArch` on an existing prefix is not supported.
- Under ARM64 Wine the loader is `lib/wine/aarch64-unix/wine.app/Contents/MacOS/wine`. That `wine.app` has no `Info.plist`.
- Preview ships DXMT in `lib/dxmt/aarch64-windows` (ARM64X DLLs) and `lib/dxmt/aarch64-unix/winemetal.so`, next to `x86_64-*` builds. An ARM64EC test build goes into the `aarch64-*` folders. Back them up first.

### 6.2 Building DXMT for ARM64EC (from upstream CI, job `build-clang-release-arm64ec-windows-cross`)

Needed in `dxmt-SC6/toolchains/` (git-ignored):

1. **llvm-mingw 20260908** (`llvm-mingw-20260908-ucrt-macos-universal.tar.xz` from `mstorsjo/llvm-mingw` releases). It provides `arm64ec-w64-mingw32-*`, which `build-arm64ec.txt` uses with `-marm64x`.
2. **Wine 11.2 for ARM64EC**: `https://github.com/3Shain/wine/releases/download/wine-11.2/wine-11.2.tar.gz`, unpacked with `--strip-components 2` into `toolchains/wine-arm64ec` (keep the existing x86_64 `toolchains/wine`).
3. **LLVM 15.0.7 for arm64 macOS**: build from the existing `toolchains/llvm-project` into `toolchains/llvm-darwin-arm64`, with the same CMake flags as the x86_64 build but `-DCMAKE_OSX_ARCHITECTURES=arm64 -DLLVM_HOST_TRIPLE=arm64-apple-darwin` (about 20–30 minutes).

Then, in a separate build folder:

```bash
export PATH="$PATH:$PWD/toolchains/llvm-mingw-20260908-ucrt-macos-universal/bin"
meson setup --cross-file build-arm64ec.txt -Denable_nvapi=true -Denable_nvngx=true \
  -Dnative_llvm_path=toolchains/llvm-darwin-arm64 -Dwine_install_path=toolchains/wine-arm64ec \
  build-arm64ec --buildtype release --prefix $PWD/install-arm64ec --strip
meson compile -C build-arm64ec
meson install -C build-arm64ec
```

The expected output is `install-arm64ec/aarch64-windows/*.dll` and `install-arm64ec/aarch64-unix/winemetal.so`. Not verified: that our fork changes (the SC6 profile, native fullscreen, unix calls 146–148) compile for ARM64EC.

### 6.3 Setting up the FEX bottle for SC6 (proposed steps)

1. Create `SC6-FEX` with the command in 6.1.
2. Map the game: the game folder is under `/Users/iphoobis`, which the `Steam` bottle reaches as `Y:`. Check the new bottle's `dosdevices`, or use `Z:` (`/`).
3. Copy the registry settings that SC6 needs into the new bottle:
   - `HKCU\Software\Wine\AppDefaults\SoulcaliburVI.exe\DllOverrides` `dinput8=native,builtin`,
   - `HKCU\Software\Wine\AppDefaults\SoulcaliburVI.exe\Mac Driver` `RetinaMode=n`.
4. Copy the game's user config (`drive_c/users/crossover/AppData/Local/SoulcaliburVI/Saved/Config`) so that the graphics settings match the `Steam` bottle.
5. Make the harness accept a CrossOver app and bottle: `CROSSOVER_APP` and `SC6_BOTTLE` are already read by `tests/run.sh`, but `LAUNCH=bundle` uses the 26.3 loader path. Use `LAUNCH=direct` for Preview, or add a Preview loader path.
6. Compare with test 08: the HUD frame rate in the Training fight, the busy share of `dxmt-encode-thr` (profiling run), and correct behavior of the x86_64 `dinput8.dll` hooks under FEX (FEX must notice the patched code; unverified).

### 6.4 Risks

- Preview is a beta. The bottle format and DXMT layout may change.
- The x64 game code runs slower under FEX than under Rosetta, I think, because FEX on macOS probably cannot use the M1's hardware x86 memory-ordering mode (`notes/dxmt.md`). Only the DXMT and Wine parts become native. The net effect is unknown until measured.
- MinHook patches in the x64 exe must be seen by FEX's code cache. If they are not, the gates or the cap removal may silently fail; check `sc6vrr.log` and the per-second step counts.
