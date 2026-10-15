# Test results

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
    - Test 08 (uncap plus gating): 90.8 fps average, up to 120.1. All 10 gates installed. In the one-second windows with at least 100 engine frames, the battle ran 60.6 steps per second on average (for example "frames=120 steps=60"). When the engine runs below 60 fps (loads, menus), there is one step per frame, so the battle slows down as in the stock game.
