# First native reconstruction experiment

Verified on 2026-10-16.

One manually reconstructed SC6 function runs as native ARM64 code and matches the original x64 instructions across 138,240 controlled inputs.
The Mac asset reader also parses one modified SC6 widget and exposes its Blueprint functions.

These results establish a small working investigation path. They do not establish a playable port or an automated recompiler.

## Executable identity

The FEX copy is located at:

`/Users/iphoobis/Library/Application Support/CrossOver/Bottles/SC6-fex/drive_c/Games/SoulCalibur 6/SoulcaliburVI/Binaries/Win64/SoulcaliburVI.exe`

It has 71,737,344 bytes and this SHA-256:

`f8904e4b04bca3b47bc52a683f6190365d2eb89ee8f44f8072759e9c5e04a553`

The earlier executable under `~/Games/SteamLibrary` has the same hash.
Therefore, the existing Ghidra analysis targets the same executable contents.
This file check identifies the supported version. It does not prove ownership.

## Asset reader

The reference project exists at `/Users/iphoobis/Documents/Unreal Projects/SC6Reference` and names engine association `4.17`.
Its content directory is empty, as expected for the selected blank project.

The supplied Mac Intel Kismet Analyzer completed these checks with `VER_UE4_17`:

1. Read UE4.17.2's `RollingGameMode.uasset` template asset. The command returned exit code 0.
2. Extract the existing boot-skip package using repak. The command returned exit code 0.
3. Read its `Advertise.uasset`, with the matching `.uexp` file beside it. The command returned exit code 0.
4. Export that asset as JSON. The command returned exit code 0 and produced valid JSON.

The SC6 package used here is `mods/build/pakchunk999-WindowsNoEditor_P.pak`.
It contains previously modified game assets. This check does not establish parsing of an untouched original package.
It required no new access to encrypted assets.

The parsed widget contains four Blueprint functions:

- `PushButton__DelegateSignature`
- `ExecuteUbergraph_Advertise`
- `OnInitialize`
- `OnHandleInput`

The parser exposed their script instructions. Graph drawing and successful compilation back into an asset remain unverified.

## Native function

The function is at `0x14030a8c0`.
The SC6 documentation names it `LuxMoveVM_GetTimeDilationScalar`.
It computes a per-character multiplier that controls how quickly selected battle work advances.

The existing Ghidra project was read with `-readOnly` and `-noanalysis`.
Ghidra recovered the function without changing the analysis database.
Disassembly confirmed that this function has no external calls.

The comparison used two separate console programs:

| Program | What it executes |
| --- | --- |
| Mac Intel reference | It reads 183 bytes of original instructions from the supported executable and runs them through Rosetta 2. |
| Native ARM64 reconstruction | It executes a manually reconstructed C++ function compiled for ARM64 with macOS 27.0 as the minimum OS. |

Both programs use constructed memory states with identical field values.
The original instruction addresses and required data addresses remain intact inside the reference program.
The programs do not launch the game or supply its other systems.

The input combinations exercise the function's different return paths, including freeze handling, scaling, and mode checks.
The test varies character kind, match state, opponent state, global mode, scale, fallback scale, freeze flag, cap, and multipliers.

Results:

- The comparison completed 138,240 input combinations.
- All 32 bits of every floating-point result matched.
- There were zero mismatching results.
- The wrapper rejected a missing original executable with exit code 2.
- The wrapper rejected an unsupported input file with exit code 2.
- The wrapper accepted the supported executable and completed the comparison with exit code 0.

This is a manual reconstruction compiled for ARM64. An automated translator remains unproven.
The test covers selected finite values and constructed memory states.
It does not cover all possible inputs, NaN values, infinity, subnormal values, or behavior inside a running battle.
NaN is a floating-point value used to represent an undefined numerical result.
Subnormal values are extremely small floating-point values whose handling can differ between platforms.

The Intel reference uses Rosetta 2. This comparison does not measure FEX behavior or prove equality with a physical Windows x64 system.
No performance comparison was made.

## Private artifacts

Generated asset data, the reconstructed function, console programs, and numerical results remain outside Git:

`/Users/iphoobis/Library/Application Support/SC6-VRR/native-investigation/`

The function experiment is in `time-scale/` within that directory.
`run_compare.py` verifies the original executable's hash before running either program.
`verified-result.json` records the comparison result.

The Ghidra pseudocode output is in the ignored path `re/logs/native-first-function.txt`.
This report contains observations and locations, rather than recovered game implementation.

## Ghidra UI state

The running Ghidra connection works, but its active project is empty.
An attempt to close it with saving enabled failed because a tool has unsaved configuration.
The configuration was preserved, so the UI project was not switched.
The separate read-only operation on `re/ghidra/SC6.gpr` succeeded.

## Next experiment

The [movement experiment](native-physics-proof.md) now compares selected paths through a function that calls this timing function.
It matches 147,456 movement steps across 18,432 constructed states.
Rotation and battle lookup branches remain unsupported.
Compare captured battle states with the original before integrating reconstructed functions into a native application.
