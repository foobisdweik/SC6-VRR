# Native movement reconstruction experiment

Verified on 2026-10-16.

The ARM64 movement reconstruction matches the original instructions across 18,432 constructed character states.
Each state advances through eight calls, so the comparison contains 147,456 movement steps.
Every compared byte matches.

This extends the [first experiment](native-first-proof.md) because the movement function calls the previously recovered timing function.
The experiment covers selected paths within the movement function. It does not establish a complete replacement for that function.

## Functions and input file

The movement function starts at `0x140306bb0`.
The existing SC6 documentation names it `LuxBattleChara_IntegratePhysics_PerTick`.
It calls `LuxMoveVM_GetTimeDilationScalar` at `0x14030a8c0`.
The timing function returns a multiplier that affects movement during this call.

The comparison requires the same original executable as the first experiment.
Its SHA-256 is `f8904e4b04bca3b47bc52a683f6190365d2eb89ee8f44f8072759e9c5e04a553`.
The private wrapper checks that hash before running either program.
This identifies a supported file. It does not establish ownership.

Ghidra recovered the movement function through the existing database with `-readOnly` and `-noanalysis`.
Direct inspection of the x64 instructions confirmed the calls, arithmetic order, branches, and writes used here.

## Paths exercised

The constructed states vary these conditions:

- The character uses ordinary movement or movement that reduces velocity on each call.
- The motion state is `0`, `0x11`, or `0x12`, which select different handling of vertical movement.
- The gravity flag is disabled or enabled.
- The bone-anchor index is `-1` or `0`, which disables or enables updates to the associated position fields.
- The flag at `+0x170a` is disabled or enabled, which changes whether the function clears `+0x96f68`.
- Twelve timing configurations select different paths through the timing function, including paths that return zero.
- Thirty-two variations supply positions, velocities, offsets, and other finite values.

The values include decimal fractions that binary floating-point numbers cannot represent exactly.
They include heights beyond both bounds enforced by the original function.
Therefore, the comparison exercises rounding and bounds rather than only exact arithmetic on simple values.

The original constants come from the supported executable.
Both programs compile with `-ffp-contract=off -fno-fast-math`.
These settings prevent the compiler from combining multiplication and addition into one operation or assuming relaxed numerical rules.
Such changes could alter rounding, so the experiment preserves the original order of operations.

## Original instructions and native reconstruction

The Intel program reads 1,310 bytes for the movement function and 183 bytes for the timing function from the original executable.
It preserves their original addresses, so the original call from movement to timing remains intact.
Rosetta 2 executes these instructions.
The program supplies constructed character memory and the required data addresses.

The ARM64 program executes manually reconstructed C++ for both functions.
It uses the same input values and constants.
It compiles with macOS 27.0 as the minimum OS.

After every call, each program writes 72 bytes from the observed character fields into its result file.
These fields include position, velocity, the cached movement delta, and the associated bone position.
The comparison includes adjacent fields that should remain unchanged.
After each eight-call sequence, both programs verify that all character bytes outside the observed ranges remain unchanged.

Results:

- Both programs completed 18,432 constructed states and 147,456 movement steps.
- Each result file contains 10,616,832 bytes.
- The files match byte for byte, with zero mismatching samples.
- Both programs reject inputs that require the unsupported rotation branch.
- Both programs reject inputs that require the unsupported battle lookup branch.

Eight movement calls do not reproduce eight complete battle ticks.
Other battle functions may change or clear these fields between calls during actual play.

## Unsupported branches

The rotation branch requires more functions and an indirect call through an object.
The experiment excludes inputs where `+0x170a` is zero and the float at `+0x190` is positive.

The battle lookup branch can call another function and inspect other battle objects.
The experiment requires the pointer at `+0x3590` to be zero.

The original instructions remain unchanged.
The harness rejects unsupported inputs before calling them. It does not replace their dependencies with fabricated return values.
Therefore, the results apply only to the selected paths.

## Movement and freeze handling

The ordinary path adds the separate offsets at `+0x150` and `+0x158` after applying the timing multiplier.
Therefore, a zero timing multiplier does not guarantee zero horizontal movement in this function.
The constructed states exercise this behavior.
Whether other battle functions clear those offsets during a freeze remains unverified.

The earlier movement document contains a broader claim that a zero freeze byte always stops movement.
The recovered instructions do not support that claim.
The freeze byte affects only certain paths through the timing function, and the separate offsets can still contribute movement.

This distinction matters for 120 Hz presentation because drawing an intermediate position must not change the game's authoritative movement.
Keep the movement calls within the planned 60 Hz battle update.
The renderer can interpolate saved positions without calling movement again for the additional displayed frames.
This experiment does not implement or validate interpolation.

## Limits and private artifacts

The comparison uses constructed finite values. It does not use captured battle states.
NaN values, infinity, and subnormal values remain outside the tested numerical scope.
The Intel reference runs through Rosetta 2, so these results do not establish equality with FEX or physical Windows hardware.
No performance comparison was made.

Recovered code, compiled programs, and numerical output remain outside Git:

`/Users/iphoobis/Library/Application Support/SC6-VRR/native-investigation/physics-step/`

`run_compare.py` checks the original file, runs both programs, checks rejected branches, and records `verified-result.json`.
The Ghidra output remains in the ignored file `re/logs/native-integrator.txt`.

The [Training-state comparison](native-live-proof.md) now matches 128 captured fighter states across idle and movement captures.
Those reads are asynchronous, so they do not establish the state at this function's entry.
The unsupported branches need their own recovery and comparisons before this function can replace the original in general use.
