# Unlocked rendering with logic at 60 Hz

The engine can render without its original 60 fps limit. Game logic still receives one update per native 60 Hz step.

## Launch

Run `dll/launch.sh` from the repository. It builds and installs the DLL, then starts the game.
It uses CrossOver Preview and the `SC6-Rosetta` bottle by default.
The default executable is:

```text
/Users/iphoobis/Library/Application Support/CrossOver/Bottles/SC6-Rosetta/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64/SoulcaliburVI.exe
```

The launcher requests a forced-on Game Mode policy and uses the custom DXMT build for direct presentation.
The user confirmed that Game Mode was NOT active, so the policy request does not establish activation.
Run `tools/gamemode/install-direct-dxmt.sh` once to install that matched graphics build, with backups.
See [Game Mode and direct presentation](gamemode.md#current-launcher-2026-10-17) for verification and remaining limits.

The default engine limit is `0`, which means unlimited. Run `dll/launch.sh 120` to set a 120 fps limit instead.
`CROSSOVER_APP`, `SC6_BOTTLE`, and `SC6_WIN64` can select another installation.
The launcher preserves a different installed sc6vrr build before replacing it.
It refuses to replace a DLL that it does not recognize as sc6vrr.

For shader capture, run `tests/shader-capture.sh launch-unlocked`.
The older `launch-fast` command is an alias. Both commands now preserve the native logic rate.
The other capture commands keep stock frame timing.

## How the timing works

A world update advances actors, battle state, world timers, particles, and other systems inside `UWorld::Tick`.
The engine calls this function before drawing its viewport. Therefore, the DLL can skip a world update while the engine still draws.

1. `SC6VRR_FRAME_MODS=1` allows the engine hooks to install.
2. `SC6VRR_UNCAP=1` clears `bUseFixedFrameRate` and returns `SC6VRR_MAX_FPS` from the engine limiter.
3. The unlock always enables the world gate, which decides whether each engine frame may advance the world.
4. The schedule uses `QueryPerformanceCounter`, which measures elapsed time independently of the engine's reported delta.
5. Each released world update receives exactly `1/60` second.
6. Frames without a released update return from `UWorld::Tick` immediately.

This replaces the earlier gates on individual battle functions.
Because all world updates receive the same fixed delta, battle timers and battle frame counters advance together.
The DLL no longer changes the return value of the function that controls hitstop.
The two clock instruction patches only count completed increments when measurements are enabled.

The schedule uses integer arithmetic, so render rates such as 144 Hz do not accumulate rounding errors.
It permits at most one world update per engine frame.
It discards missed whole steps after a stall, so the battle does not accelerate afterward.
A pause longer than 250 ms starts a fresh interval.

Below 60 rendered frames per second, battle logic slows as it does in stock SC6.
This implementation does not run several world updates inside one engine frame.
It does not interpolate positions or poses between logic updates, so character motion still changes at 60 Hz.
Online play and very short input taps remain unverified.

## Installation checks

The DLL requires the supported executable hash and checks the first bytes of all three required engine functions.
It creates all required hooks before enabling any of them. It enables the limiter hook last.
If a required hook fails, it removes the required hooks that it already created.
Therefore, a missing world gate cannot leave the rendering unlock active.
Shader capture hooks remain independent of the engine hooks.

## Validation

Run `tests/check-measurements.sh --wine` to build the DLL and execute the checks.

- The Python checks validate measurement summaries and process identity.
- The native scheduling check covers eight render rates from 60 to 1,000 fps.
- The scheduling check covers stalls, pauses, a changed timer origin, and rendering at 45 fps.
- The Wine check exercises the real hooks with generated executable functions, including failure after partial creation.
- The Wine check advances two fake worlds through ten seconds of rendering at 240 fps.
- The Wine checks preserve the earlier coverage of clocks, function arguments, thread names, and shader capture.

Training validation completed on 2026-10-17 in the `SC6-Rosetta` bottle.
The game used CrossOver Preview and the executable path above.
The launcher requested no engine limit and disabled shader capture.
The user confirmed that Training was ready before the measurement started.

| Measure | Result over 31.508 seconds |
| --- | --- |
| Engine frames | 2,309, or 73.283 per second |
| Completed battle updates | 1,839, or 58.366 per second |
| Executed increments at R1 | 1,839, matching the battle updates |
| Executed increments at R2 | 0 |
| Counter resets | 0 |

The engine exceeded 60 frames per second while the battle remained below its native 60 Hz limit.
Stalls reduced the battle's average rate because the schedule discards missed whole steps.
These counts measure engine frames, not independently observed display presentations.
The test did not establish smoother character motion, online compatibility, or behavior for every attack.

Local evidence: `tests/runs/unlocked-rendering-20261017/training-summary.json`, `sc6vrr.log`, and `checks.log`.
The live run used the same timing code as the final build.
A later refactor extracted the hook installation helper so the Wine tests could check its failure paths.
