# What sets the 60 fps cap

Summary: the cap is stock UE4 4.17 frame limiting. No SoulCalibur-specific code was found that sets it. The value 60 must come from engine config inside the encrypted `pakchunk0` (or from a frame-rate setting applied at startup). Which of the three stock mechanisms below is active is **not yet known**. Step 3's loader DLL will read it at runtime.

Exe: SHA-256 `f8904e4b…a553`. Addresses are absolute (image base `0x140000000`). Found with `re/probe.sh` (headless Ghidra) and radare2 on 2026-10-15.

## The frame limiter

`UEngine::UpdateTimeAndHandleMaxTickRate` at `0x142189040`. Its embedded log string names `bUseFixedFrameRate` and `FixedFrameRate` and passes their values, which confirms the field offsets below. Each engine frame, it does this:

1. It measures the real time since the last frame (`QueryPerformanceCounter`).
2. It calls `GetMaxTickRate(DeltaTime, true)` through vtable offset `+0x270`.
3. If `bUseFixedFrameRate` is set, it replaces that result with `FixedFrameRate`.
4. If the frame finished early, it sleeps and then spins until `1 / MaxTickRate` seconds have passed.
5. If `bUseFixedFrameRate` is set, it also sets the engine time to `last time + 1 / FixedFrameRate`. So `FApp::DeltaTime` becomes exactly `1 / FixedFrameRate`, not the measured time.

A separate path is used when `FApp::bUseFixedTimeStep` (`0x14416a944`) or `0x14416a943` is set. In that case, the engine adds a fixed delta (`0x144070b98`) with no waiting. The startup code `0x14039df70` sets `0x14416a944` only for the command-line flags `-UseFixedTimeStep` or `-Deterministic`, so this path is off in normal play.

## How the maximum rate is chosen

`UEngine::GetMaxTickRate` is at `0x142177dc0` (`UEngine` vtable slot `0x1438c4510`). `UGameEngine::GetMaxTickRate` (`0x141e30010`, slot `0x1438c6d60`) is stock: it adds a cap only for demo (replay) playback, and otherwise uses the `UEngine` result. No Lux subclass of the engine exists; the exe has no `LuxGameEngine` string.

`UEngine::GetMaxTickRate` works like this:

1. Rate smoothing applies when `IsAllowedFramerateSmoothing()` (vtable `+0x290`) returns true. Then the rate is `1 / RunningAverageDeltaTime` (`+0xAB0`), clamped to `SmoothedFrameRateRange`. The lower bound type is at `+0x650` and its value at `+0x654`. The upper bound type is at `+0x658` and its value at `+0x65C`. Type `2` means open (no bound).
2. If `t.MaxFPS > 0`, `t.MaxFPS` **replaces** the result. It does not take the smaller value. So `t.MaxFPS` overrides smoothing.
3. The debug variable `t.UnsteadyFPS` would return a random rate (8–32). Its default is 0.

## Fields and globals

| Item | Location | Notes |
| --- | --- | --- |
| `bUseFixedFrameRate` | `UEngine+0x648`, bit mask `0x40` | Confirmed by the log string in `0x142189040` |
| `FixedFrameRate` | `UEngine+0x64C` (float) | Property table `0x14235b710`. The engine constructor sets 30 (`0x141e24b77`). |
| `SmoothedFrameRateRange` | `UEngine+0x650` (`FFloatRange`, 16 bytes) | Property table and `GetMaxTickRate` |
| `bSmoothFrameRate` | Bitfield near `+0x648` | Exact bit not found yet |
| `MinDesiredFrameRate` | `UEngine+0x73C` (float) | Used by `0x141e62680`, probably for the dynamic resolution hint. Not part of the cap. |
| `RunningAverageDeltaTime` | `UEngine+0xAB0` (float) | |
| `GEngine` | `0x1443b3068` | Read as the engine pointer in `0x141e62680` (probably `GEngine`) |
| `t.MaxFPS` console object | `0x1443b3570` (the `IConsoleVariable*`) | Registered at `0x140270a40` |
| `t.MaxFPS` value | `*(float*)*(void**)0x1443b3578` | Game-thread value |
| `UEngine::GetMaxFPS` / `SetMaxFPS` | `0x142177d70` / `0x142185da0` | Vtable `+0x278` / `+0x280` (`UGameEngine` vtable base `0x1438c6af0`) |
| `UGameUserSettings::FrameRateLimit` | `this+0xA4` (float) | Property table `0x14241f850` |

No code stores 60 or 62 into `FixedFrameRate`, `FrameRateLimit`, or the smoothing upper bound. The search used `mov dword [reg+disp], imm32`.

## Which mechanism is active (open)

The stock options that give 60 fps are:

1. **`bUseFixedFrameRate=True`, `FixedFrameRate=60`.** This is what fighting games on UE4 commonly use. If it is active, every engine delta is exactly 1/60 s. That includes the UI and particle deltas that the docs call "real time".
2. **`bSmoothFrameRate=True`** with an upper bound of 60 (or 62).
3. **A `t.MaxFPS` / `FrameRateLimit` of 60** set somewhere other than the user's `GameUserSettings.ini`, which has `FrameRateLimit=0`.

To decide: the loader DLL logs `GEngine+0x648` (whole word), `+0x64C`, `+0x650..+0x65F`, and the `t.MaxFPS` value once the engine is up.

## How to raise the cap (for Part B)

If the battle simulation is gated, the cap is no longer what keeps the game at the right speed. Whatever the active mechanism is, these steps raise the cap to 120:

1. Clear `bUseFixedFrameRate` (bit `0x40` at `GEngine+0x648`). This also gives real, measured deltas to the parts of the game that use time.
2. Set `t.MaxFPS` to 120, for example with `FrameRateLimit=120.000000` in `GameUserSettings.ini`, or through `SetMaxFPS`. Because `t.MaxFPS` replaces the smoothing result, the smoothing range does not matter.

First, try an ini-only test: `[/Script/Engine.Engine]` with `bUseFixedFrameRate=False` in `Saved/Config/WindowsNoEditor/Engine.ini`, plus `FrameRateLimit=120.000000`. It is unverified whether the shipping build reads those user overrides. If it does not, the DLL writes the fields directly.
