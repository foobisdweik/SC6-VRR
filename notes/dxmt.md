# DXMT changes for SC6

Fork: `/Users/iphoobis/Projects/dxmt-SC6` (`foobisdweik/dxmt-SC6`). On 2026-10-15, all six branches were identical to upstream `3Shain/dxmt` (GitHub compare: ahead 0, behind 0). `foobisdweik/dxmt-exp` `main` was identical too. So the DXMT in CrossOver (built 2026-10-13) is very probably stock upstream.

Upstream `AGENTS.md` says that AI must not generate code for contributions to that project. So changes made with Claude stay in this fork and are never sent upstream.

## What SC6 uses

From the game's `DefaultEngine.ini` and `DefaultDeviceProfiles.ini` (in `pakchunk0`):

- Direct3D 11, shader model 5 (`+TargetedRHIs=PCD3D_SM5`). DXMT's D3D11 path is used.
- FXAA (`r.DefaultFeature.AntiAliasing=1`). There is no temporal anti-aliasing, so no frame history to break, and MetalFX *spatial* upscaling fits.
- `r.TextureStreaming=0`.
- `ScreenPercentage` and motion-blur settings exist only in the PS4 and Xbox One device profiles.
- The DXMT shader cache for SC6 (`shaders_320.db`) is 35 MB.

## Relevant DXMT code (upstream `main` 186a232)

| Area | Code | Behavior |
| --- | --- | --- |
| Per-game defaults | `src/util/config/config.cpp` `g_profiles` | Profiles are chosen by an exe-name regex. The app profile is merged after the user config. |
| Present pacing | `src/d3d11/d3d11_swapchain.cpp` `Present1` | Minimum frame time is `max(SyncInterval / rate, 1 / preferredMaxFrameRate)`. If it is above 0, DXMT uses `presentDrawable:afterMinimumDuration:` (`dxmt_context.cpp:1101`). |
| Display sync | `src/dxmt/dxmt_presenter.cpp:19` | `displaySyncEnabled` is always `false`. There is no config option. |
| Frame latency | `d3d11_swapchain.cpp:34` | `kSwapchainLatency = 1`. Good for a fighting game; keep it. |
| Pipeline (PSO) compile | `src/d3d11/d3d11_pipeline.cpp` | Compiled on a thread pool at the first draw that needs it. The encoding thread then waits in `GetPipeline()` (`d3d11_context_impl.cpp:4841`, "may block"). With latency 1, a slow compile delays that frame's present. |
| PSO archives | `src/winemetal/unix/winemetal_unix.c` | Render and compute PSO creation accept `MTLBinaryArchive` objects for lookup and for saving. `Metal.hpp` exposes `newBinaryArchive` and `serialize`. Nothing in `d3d11` or `dxmt` uses them. |

## Decided changes (2026-10-15)

All changes go on a branch in `dxmt-SC6`.

1. **SC6 app profile.** Add `SoulcaliburVI\.exe` to `g_profiles` in `src/util/config/config.cpp`, with:
   - `d3d11.preferredMaxFrameRate=120` (ProMotion pacing),
   - the new MetalFX key from item 5,
   - `d3d11.metalSpatialUpscaleFactor=2`.

   On a 60 Hz display, the 1/120 s minimum frame time does not limit anything, because the display is slower.
2. **Display-sync option.** Add a config key (proposed name `dxgi.metalDisplaySync`, default `False` as today) that sets `display_sync_enabled` in `dxmt_presenter.cpp`. The step 6 tearing test needs it.
3. **Persistent PSO archive plus warm-up.**
   1. Pass one `MTLBinaryArchive` per game to every PSO creation, both for lookup and for saving, and save it at exit.
   2. Record each PSO description (shader hashes and state) to disk.
   3. At the next launch, compile the recorded set on the thread pool before it is needed.

   At 120 fps the frame budget is 8.3 ms. With a frame latency of 1, any compile that blocks the encoding thread for longer drops a frame. Measure first: `DXMT_LOG_LEVEL=trace` logs "Compiled 1 PSO".
4. **Display-time feedback, behind a switch.** Add a config key (proposed `d3d11.reportPresentTiming`, default `False`). When it is on, DXMT records each frame's actual display time (`MTLDrawable` presented handler) and exposes it through a private interface on the swapchain. The SC6 DLL uses CPU time by default, and uses the display time only when it finds the interface and the switch is on.
5. **MetalFX spatial upscaling by default.** Upstream turns it on only with the environment variable `DXMT_METALFX_SPATIAL_SWAPCHAIN=1` (`d3d11_swapchain.cpp:1124`), so a profile cannot enable it. Add a config key (proposed `d3d11.metalSpatialUpscale`) that the swapchain checks as well as the variable.
   - Target: the game renders at 1728 × 1117 in borderless windowed mode, and MetalFX upscales by 2 to the panel's 3456 × 2234 pixels.
   - The `Steam` bottle has `RetinaMode="y"` (`user.reg`, `[Software\\Wine\\Mac Driver]`). In that mode Wine reports the full 3456 × 2234 pixels, so the game would probably render at that size, and a 2× upscale would overshoot.
   - So SoulcaliburVI.exe needs Retina mode off for that app only: `[Software\\Wine\\AppDefaults\\SoulcaliburVI.exe\\Mac Driver]` with `"RetinaMode"="n"`. The other games in the bottle keep the current setting.
   - Unverified: that the Mac driver reads this per-app key, and that the layer's drawable ends up at 3456 × 2234 (the presenter multiplies the layer's `contentsScale` by the factor). Check both in step 6.

Not planned: async PSO compile that skips draws until the PSO is ready. In a fighting game, a skipped draw can hide a hit effect or a character for a frame.

## Upstream branches: do not merge

| Branch | Commits ahead of `main` | What it changes | For SC6 |
| --- | --- | --- | --- |
| `dxil-dev`, `feat/dxil-1` (same content), `feat/dxil-0` | 40, 40, 21 | DXIL (D3D12 shader model 6) conversion. It also refactors the shared DXBC converter (`dxbc_converter*.cpp`, `air_operations.cpp`, signatures). | No gain, because SC6 uses D3D11 with DXBC shaders. The shared converter changes risk shader bugs and would probably invalidate the 35 MB shader cache. |
| `feat/d2d-interop` | 2 | DXGI surface for Direct2D interop | Not used by UE4 |
| `chore/idl` | 4 (22 behind) | D3D12 IDL and CI | Not relevant |

One commit from the DXIL branches is worth considering on its own: `a584b53` "fix(airconv): fix potential leaks of `sm50_*` objects". It fixes small leaks in the DXBC converter that SC6 uses (2 files, +52/−62), and it applies cleanly to `main` (tested with `git cherry-pick --no-commit`, then reverted). The gain is small: one leaked error object per converted shader.

## Measure before changes 3–5

During the step 1 baseline, record GPU time per frame (Metal HUD), the PSO compile count and time per match (DXMT trace log), and hitch frames longer than 8.3 ms.

## Runtime: CrossOver 26.3 (Rosetta) or CrossOver Preview 27.0 (FEX)

Installed on 2026-10-15:

- CrossOver 26.3 (`/Applications/CrossOver.app`) runs the whole x86-64 Wine process under Rosetta 2. Its `lib/dxmt` has `aarch64-windows` folders, but this version cannot use them.
- CrossOver Preview 27.0.0.41069 (build 20261006) ships FEX (`libarm64ecfex.so`, `libwow64fex.so`) and DXMT with `aarch64-windows` (ARM64EC) builds. With FEX, x86-64 game code is emulated by FEX, and ARM64EC DLLs such as DXMT run as native ARM code.

Decision: develop on CrossOver 26.3 with Rosetta, and compare the Preview with FEX in the step 1 baseline.

1. The SC6 DLL patches x86 code at runtime. Under CrossOver, Rosetta already runs injected x86 hooks (BepInEx works, see `~/Ops/KNOWN.md`). FEX should handle patched code too, but that is unverified here.
2. SC6's game thread is x86 code under both translators. Rosetta can use the M1's hardware x86 memory-ordering mode. I think FEX on macOS cannot, so it must add memory barriers, which usually makes multithreaded x86 code slower. This is unmeasured.
3. FEX helps the graphics side: DXMT's own CPU work (draw encoding, PSO setup) would run as native ARM64EC code instead of translated x86 code. That matters only if the baseline shows DXMT's encoding thread limits the frame rate.
4. Any DXMT change must then be built twice (`x86_64-windows` and ARM64EC, see `build-arm64ec.txt`), and the Preview is a beta.

Compare both runtimes on the same training-mode scene: CPU and GPU frame time, and frames over 8.3 ms.
