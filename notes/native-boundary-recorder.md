# Recorder for movement inputs and outputs

Verified in `SC6-fex` on 2026-10-16. The temporary recorder was removed after the game exited.

The recorder captured 64 actual movement calls in Training mode.
The ARM64 reconstruction and the original instructions each reproduced every observed output byte for those calls.

The next comparison needs the inputs used by an actual movement call and the outputs that call produces.
The earlier [Training captures](native-live-proof.md) read memory independently of the game's updates, so they cannot establish those two states.

## Prepared recorder

The recorder intercepts the movement function at `0x140306bb0`.
Interception means another function receives the call, performs the recording, and calls the original function.
The recorder calls the original function once and saves the relevant outputs after it returns.

The intended recording sequence is:

1. Read the current character, its opponent, and the required global fields before calling the original function.
2. Call the original movement function.
3. Record the same 72 output bytes used by the earlier comparisons.
4. Save the record from a separate thread.

The capture occurs around the call on the thread that executes movement.
This reduces the uncertainty caused by reading between unrelated updates.
The reads do not freeze other threads, so they do not establish an atomic copy of all game memory.

The recorder waits until an external `arm.flag` file enables capture.
Therefore, the game can reach Training mode before capture begins.
It limits sampling to two calls per second for each fighter and stops after 64 records.
Each record contains 1,243,304 bytes, including the inputs and the observed outputs.

The recorder allocates about 80 MB for its records.
Copying the inputs can affect game timing, so this run must not be used as a performance benchmark.
The interception remains active until the game exits, even after recording ends.

The recorder checks the executable's SHA-256 and the target's instruction prefix before installing interception.
It does not enable the existing frame-rate mod or change the simulation schedule.

## Checks completed

The interception library is MinHook, which the existing project already uses.
A separate x64 Windows program tested it inside `SC6-fex`.
The test intercepted 1,000 calls across two threads and checked the outputs after each call.
All checks passed, and disabling interception stopped further recording.

The recorder DLL completed these checks:

- It compiled with warnings treated as errors, except for the expected Windows function-pointer casts.
- Its six exported functions were available through FEX.
- Its `DirectInput8Create` function successfully created an input object through the system DLL.
- It rejected the unsupported test executable before installing interception.
- Its installation tool refused to change the game folder while SC6 was running.

These checks established preparation and limited FEX compatibility before the game test.
The completed game test is described below.

Installation completed after the game exited.
The installed DLL matches the prepared file, and the existing `dinput8.dll.bak` remains present.
The bottle already sets `dinput8` to `native,builtin`, which permits loading the recorder before the system DLL.
Capture was enabled after Training mode loaded and stopped after saving all 64 records.

## Completed game test

The game's recorder log reports successful initialization, creation, and activation of interception.
It saved all 64 records with `valid=1` and then reported completion.
Each fighter contributed 32 records.
All calls ran on the same Windows thread, with identifier `320` in this session.

Each record contains the inputs copied before the original movement call and 72 output bytes read after that call returned.
The comparison programs restore those inputs, adjust the pointers used by the supported paths, and execute one movement call.
They compare their result with the output observed inside FEX.

| Comparison | Accepted calls | Excluded calls | Calls with different outputs |
| --- | --- | --- | --- |
| Original x64 instructions through Rosetta 2, compared with the game in FEX | 64 | 0 | 0 |
| ARM64 reconstruction, compared with the game in FEX | 64 | 0 | 0 |
| ARM64 reconstruction, compared with the original x64 instructions | 64 | 0 | 0 |

All 4,608 observed output bytes matched in each comparison.
Eleven calls changed position.
Both comparison programs verified that character bytes outside the observed ranges remained unchanged during their isolated calls.
The recorder checks 72 output bytes in the running game, so it does not establish that all other live memory remained unchanged.

The recorded motion codes were `0` in 56 calls, `2` in four calls, and `7` in four calls.
Every recorded input height was zero, so this capture does not validate airborne movement.
No record required the unsupported rotation path or a nonempty lookup structure.
Therefore, this result does not validate those paths.

This is a comparison against outputs produced inside the running game.
It establishes agreement for these 64 calls and their observed fields.
It does not establish complete battle behavior, rendering, or a playable native port.

## Files and next run

The prepared files remain outside Git:

`/Users/iphoobis/Library/Application Support/SC6-VRR/native-investigation/boundary-capture/`

`manage_recorder.py` supports `install`, `status`, `arm`, and `remove`.
It targets only the SC6 executable in the `SC6-fex` bottle.
It preserves the existing `dinput8.dll.bak` and refuses to replace another active DLL.
Removal checks the installed recorder's hash before deleting it.

The completed session is `session-20261016-175520-316/` inside the private directory.
Its `comparison.json` records the results and hashes of the 64 records.
Its `input-summary.json` records the motion codes, positions, and thread identifiers.
`run_boundary.py` checks the original executable and capture completion before comparing the records.

The diagnostic run requires these steps:

1. Quit SC6 so the recorder can be installed for the next launch.
2. Install the prepared recorder and launch SC6 through `SC6-fex`.
3. Enter Training mode, then enable capture.
4. Exercise walking, jumps, and attacks while the recorder collects inputs and outputs.
5. Compare the supported recorded calls with the original instructions and the ARM64 reconstruction outside the game.
6. Quit SC6 and remove the recorder after the diagnostic run.

The capture and comparison steps are complete.
The game exited, and the recorder was removed after its installed hash was checked.
The active `dinput8.dll` is absent, and the existing `dinput8.dll.bak` remains present.

Recorded calls that require an unsupported rotation or lookup path need separate recovery.
They must be reported as unsupported rather than counted as successful comparisons.
