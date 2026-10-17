# SC6-VRR plan

## Current priority (2026-10-16)

The active goal is a native Apple silicon program built in a clean-room shape (decided 2026-10-16):

1. The analysis side studies the original executable and the running game, then writes specifications in its own words.
2. The implementation side writes new code from those specifications only, in a separate repository.
3. The shipped program contains only that new code and permissively licensed libraries.
4. At runtime, the program loads assets from the user's own installation.

The first target is battle logic that matches the original game's behavior and timing.
HDR, ProMotion, interpolation, and changes to game timing are deferred.
Metal rendering moved back into scope on 2026-10-16, after the clean-room plan was recorded.

Tests must cover connected battle behavior across recorded sequences for both fighters.
Cover movement, airborne behavior, attacks, hits, defense, recovery, freeze handling, and resets as the specified sections permit.
Report unsupported behavior explicitly, and compare the outputs with the original game.
Increasing the number of isolated function calls does not satisfy this requirement.

The earlier ARM64 reconstructions follow Ghidra's reconstruction of the original code.
So they are analysis material: they may check the specifications, but they never enter the shipped program.
The current proof matches 64 actual movement calls, including 11 calls that change position. It is a preliminary analysis result.

Two legal questions block any public release, but not the analysis and implementation work:

- The program must decrypt the user's pak files to load assets. Whether that is lawful circumvention is unsettled.
- The game's license and the Steam terms have not been checked for reverse-engineering clauses.

See [the active native plan](notes/native-port.md) and [the broader test plan](notes/native-battle-tests.md).

## Earlier VRR design (deferred)

The earlier goal was rendering at up to 120 fps while preserving 60 Hz battle logic and enabling ProMotion pacing.
The sections below preserve that work. They do not set the current work priority.

## 1. What we know (verified 2026-10-15)

| Fact | How it was checked |
| --- | --- |
| The exe is the build that the docs describe. SHA-256 `f8904e4b…a553`, 71,737,344 bytes. | `shasum -a 256` matches `SC6-docs/docs/cookbook/dotvanisher.md`. |
| Doc addresses are valid for our exe. | The bytes at R1 `0x1403E1FEA` and R2 `0x1403E2033` are `ff 83 a4 03 00 00` (`inc dword [rbx+0x3A4]`), as `SC6-docs/docs/sc6/replay-system.md` says. |
| The engine is UE4 4.17.2, run through CrossOver 26.3 (bottle `Steam`) and DXMT (a Direct3D 11 to Metal translation layer). | Docs, bottle files, and the DXMT shader cache folder `dxmt/SoulcaliburVI.exe`. |
| The user config does not set the cap. `GameUserSettings.ini` has `FrameRateLimit=0.000000` and `bUseVSync=False`. | Read from the bottle's `AppData/Local/SoulcaliburVI/Saved/Config`. |
| The exe contains the UE4 cap settings `t.MaxFPS`, `bUseFixedFrameRate`, `FixedFrameRate`, `bSmoothFrameRate`, and `SmoothedFrameRateRange`. | UTF-16 string search of the exe. |
| `pakchunk0` has an encrypted index (pak version 4). | `repak info` fails with "pak is encrypted but no key was provided". |
| The installed DXMT reads `d3d11.preferredMaxFrameRate`. It passes the minimum present interval (`SyncInterval / refresh`, or `1 / preferredMaxFrameRate`) to the Metal present call. | Strings in CrossOver's `lib/dxmt/x86_64-windows/d3d11.dll` and upstream `src/d3d11/d3d11_swapchain.cpp`. |
| The display is the built-in 16-inch Liquid Retina XDR (ProMotion, 24–120 Hz). | `system_profiler SPDisplaysDataType`. |

### The simulation is frame-locked

The docs in `SC6-docs/docs/` (submodule `FottenSC/SC6ModdingDocs` at `cda3eba`) agree that one battle tick advances the game by exactly one 60 Hz frame:

1. `LuxBattle_PerFrameTick` (`0x1402DBC60`) runs one fixed frame per call. Velocities, decays, and move frame counters are per tick, and no engine delta time is used.
2. The replay master clock (`InputLog+0x3A4`) increases by 1 every UE4 frame (R1/R2 above).
3. The battle manager's catch-up loop (`0x1403FE520`) runs `master clock − last applied frame` simulation steps, and it reads one input entry per step.
4. So if the engine renders 120 frames per second, the master clock also advances 120 times per second, and the fight runs at double speed.

Some parts already use real time: UMG widgets, particles, audio, and the non-battle animation blender use UE4's `World::DeltaTime` (`SC6-docs/docs/sc6/movement.md:275`).

## 2. Design

There are two independent parts.

### Part A: decouple rendering from the simulation (a native DLL)

- **Fixed-step clock.** Each engine frame adds the real elapsed time to an accumulator. The DLL then releases `floor(accumulator / (1/60 s))` simulation steps, which is usually 0 or 1 at 120 fps. The simulation therefore advances 60 times per second at any render rate.
- **Gate points.** Gate each place where a frame advances the simulation, so that it runs only when a step is released:
  1. the sim core `LuxBattle_PerFrameTick` (doc "Site 9"; HorseMod's WorldTickGate already uses this credit model),
  2. the master clock increments R1 and R2,
  3. the actor ticks that advance input and round state (doc Sites 11, 20, and 21),
  4. the `ALuxBattleManager::Update_Impl` timers (Site 21b), after checking whether they use real delta time.
- **Interpolation.** Without interpolation, 120 fps shows each simulation frame twice, so the motion does not look smoother. The DLL keeps the previous and current simulation states and blends them with `alpha = accumulator / (1/60 s)` for the render-only data:
  1. the character render position (`chara+0x2090`) and the game-thread pose (`chara+0xC0`),
  2. animation lanes, using the fractional frame in `lane+0x08`,
  3. the camera point of view that `UWorld::Tick` commits each frame.
- **Input timing (decided 2026-10-15).** The game polls the controller on every rendered frame, so the polling rate follows the render rate. Timing windows stay in 60 Hz simulation frames.
  1. UE4 already polls input on every engine tick, so the polling path is not gated. Only the code that gives input to the simulation is gated.
  2. Between two simulation steps, the DLL merges all polled samples. A press or release that happens between steps is kept, so a tap shorter than one step (16.7 ms) is not lost.
  3. The next simulation step reads the merged state, using the stock input path and the stock per-frame input record.
  4. As a result, a 10-frame window still means 167 ms. Input reaches the simulation up to about 8 ms sooner at 120 fps, because the newest sample is at most one render frame old.
  5. Replays and online sync still see one input entry per 60 Hz frame.
- **Hitstop and slow motion** stay inside the simulation (`GetTimeDilationScalar`, `0x14030A8C0`), so the fixed step does not change them.
- **Online play** stays on stock behavior. The input-delay netcode counts frames from the master clock, so the DLL turns itself off in online modes.
- **Loading.** A proxy `dinput8.dll` (the exe imports it) goes in `Binaries/Win64`. It forwards the real exports and installs MinHook detours. It refuses to hook if the exe's SHA-256 or a target's first bytes differ from what we expect.

### Part B: raise the cap and turn on ProMotion pacing

1. Find what sets the 60 cap. In Ghidra, follow the references to the cap strings into `UEngine::GetMaxTickRate`, and check for a Lux-specific limiter. The DLL logs the live `GEngine` values of `bUseFixedFrameRate`, `FixedFrameRate`, `bSmoothFrameRate`, and `SmoothedFrameRateRange`.
2. Raise the cap to 120. Try the least invasive method first:
   1. a `[/Script/Engine.Engine]` override in `Saved/Config/WindowsNoEditor/Engine.ini`,
   2. `t.MaxFPS 120` through the console exec path that still works in shipping (`SC6-docs/docs/sc6/line-batching.md`),
   3. a patch to the cap function inside the DLL.
3. Set ProMotion pacing and MetalFX spatial upscaling as the defaults through an SC6 profile in the `dxmt-SC6` fork, and turn off Retina mode for SoulcaliburVI.exe only. See [`notes/dxmt.md`](notes/dxmt.md) for all DXMT changes. ProMotion behavior is unverified until the test in step 6.
4. Ship an install script that writes these defaults, the DLL, and the per-app Wine override `SoulcaliburVI.exe → dinput8 = native,builtin`. The script backs up every file that it changes.

## 3. Tools

| Tool | Purpose | State |
| --- | --- | --- |
| Ghidra 12.1.4 + JDK 21 | Decompile and cross-reference the exe | Installed. Project `re/ghidra/SC6` is being analyzed (log in `re/logs/`). |
| radare2 | Quick byte and import checks | Installed |
| mingw-w64 GCC 16.2, cmake, ninja | Build the Windows x64 DLL | Installed. Test build with MinHook succeeded. |
| MinHook v1.3.4 | x64 function hooks | Added as submodule `third_party/minhook` (BSD-2-Clause) |
| repak v0.2.3 | Build unencrypted `~mods` override paks, if needed | Installed from `trumank/repak` |
| CrossOver 26.3 + DXMT (custom build, 2026-10-13) | Run the game, Metal pacing | Installed |
| Metal Performance HUD (`MTL_HUD_ENABLED=1`) | Measure fps and present interval | Built into macOS |
| gh | Push to `foobisdweik/SC6-VRR` | Logged in |

Not needed: UE4SS (our own small loader has fewer parts), the pak AES key (we read the cap at runtime instead), and ghidra-mcp (headless Ghidra scripts are enough; it is optional).

## 4. Steps

1. **Baseline.** Partly done 2026-10-15 on CrossOver 26.3 (`notes/test-results.md`); the CrossOver Preview comparison is still open. Run the game with the Metal HUD on, on CrossOver 26.3 and on CrossOver Preview 27.0 with FEX (see [`notes/dxmt.md`](notes/dxmt.md)). Record the fps, the present interval, and the round timer length in training mode. This gives the stock numbers to compare with.
   - Measurement tools updated and tested live on 2026-10-15. Test 10 measured 77.1 engine frames/s and 59.52 completed battle advances/s. The battle counter total matched master-clock increments. Most elapsed frame time lies outside `UWorld::Tick`. The CPU or GPU limit remains unverified. See [test 10](notes/test-results.md#test-10-measurement-without-sample).
   - Cases 11 and 13 completed on 2026-10-16, but their actions differed. The four idle repeats on Kaer Morhen then completed with native exports. DXMT measured 80.077 fps with the detail timers enabled and 80.017 fps with them disabled. This pair shows no clear timer penalty. D3DMetal measured 86.180 and 86.611 fps, but the user reported a black floor in the latter test. Its timer comparison remains inconclusive. Different layer dimensions and presentation prevent attributing the cross-implementation difference solely to translation. See [the idle comparison](notes/idle-comparison.md).
   - The follow-up identified the reproduced full-core thread as SC6's `RenderThread 2`. Its Windows thread record matched the exact OS counter handle at 99.286% CPU. The game thread used 26.749%, and DXMT's encoding thread used 41.222%. Precise function costs remain unknown because the profiler reconstructed Windows stacks incorrectly. See [the thread identification](notes/thread-identity.md).
2. **Find the cap.** Done 2026-10-15; see [`notes/frame-cap.md`](notes/frame-cap.md). `DefaultEngine.ini` in the pak sets `bUseFixedFrameRate=True` and `FixedFrameRate=60`.
3. **Loader DLL.** Done 2026-10-15 (`dll/`, test 07). Build the proxy `dinput8.dll` with the hash check and logging only. This proves that it loads under CrossOver before any hook goes in.
4. **Cap test.** Done 2026-10-15 (test 07: up to 120 fps, fight too fast). Raise the cap with no gating. The fight should run fast, which confirms the frame-locked model above.
5. **Fixed-step gating and input merge.** Gating done 2026-10-15 (test 08: 120 frames, 60 steps per second, normal speed and input). The input merge is still open. Add the accumulator, the gates, and the merge of input samples between steps. Done means:
   1. `nMasterClock` and `g_LuxBattle_FrameCounter` advance by 60 per second at 120, 90, and 45 fps.
   2. A tap shorter than 16.7 ms registers at 120 fps.
   3. Motion inputs still have the same frame windows, as checked against the training-mode input display.
6. **ProMotion default.** DXMT side done 2026-10-15 (SC6 profile, native fullscreen, BGR10A2 layer gives "Direct"); the present-interval check across 24–120 Hz is still open. Apply the DXMT setting. In the Metal HUD, check that the present interval follows the frame time between 24 and 120 Hz, in both fullscreen and windowed mode.
7. **Interpolation.** Blend render position, pose, animation frame, and camera. Done means visibly smooth 120 fps motion with no extra frame of delay.
8. **Determinism test.** Record a replay at 120 fps and play it back with the DLL off. If the outcome is identical, the simulation did not change.
9. **Install script and README.**

## 5. Open questions

None at the moment.
