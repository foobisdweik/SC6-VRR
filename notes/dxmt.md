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

## Ranked changes

1. **SC6 app profile (small, do first).** Add `SoulcaliburVI\.exe` to `g_profiles` with `d3d11.preferredMaxFrameRate=120`. ProMotion pacing then becomes the default for this game only, with no bottle environment variables. On a 60 Hz display, the 1/120 s minimum does not limit anything, because the display is slower.
2. **A display-sync option (small, for the step 6 test).** Add a config key (for example `dxgi.metalDisplaySync`) that sets `display_sync_enabled`. Without it, we cannot test whether `displaySyncEnabled = NO` causes tearing with VRR.
3. **Persistent PSO archive plus warm-up (medium, likely the biggest gain in smoothness).** Pass one `MTLBinaryArchive` per game to every PSO creation, both for lookup and for saving, and save it at exit. Also record each PSO description (shader hashes and state) to disk. At the next launch, compile the recorded set on the thread pool before it is needed (like DXVK's state cache). At 120 fps the frame budget is 8.3 ms, so any compile that blocks the encoding thread for longer drops a frame. This is unverified until we measure how many PSOs compile during a match, and how long they take: `DXMT_LOG_LEVEL=trace` logs "Compiled 1 PSO".
4. **Display-time feedback for interpolation (medium, optional).** DXMT could report each frame's actual display time (`MTLDrawable` presented handler) through a private interface. The SC6 DLL could then compute the interpolation blend from the predicted display time instead of CPU time, which may reduce judder when the refresh rate varies. Value unverified; try CPU time first.
5. **MetalFX spatial upscaling (config only, if GPU-bound).** `DXMT_METALFX_SPATIAL_SWAPCHAIN=1` with `d3d11.metalSpatialUpscaleFactor` between 1.0 and 2.0. It fits because SC6 uses FXAA. Use it only if the step 1 baseline shows the GPU cannot reach 120 fps at native resolution.

Not recommended: async PSO compile that skips draws until the PSO is ready. In a fighting game, a skipped draw can hide a hit effect or a character for a frame.

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
