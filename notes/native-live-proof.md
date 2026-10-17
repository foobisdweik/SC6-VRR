# Movement comparison with captured Training states

Verified on 2026-10-16.

The user loaded Training mode through the `SC6-fex` bottle.
A separate reader found the running game and read both fighters without pausing or patching the game.
The comparisons match the native reconstruction against original instructions using 128 captured fighter states across two captures.

## Capture method

The reader runs as an x64 Windows program inside the same bottle.
It uses `OpenProcess` and `ReadProcessMemory` with permission to query the process and read its memory.
It does not request permission to write memory or change threads.

The reader verified the loaded image header and found the image at `0x140000000`.
The original file matches SHA-256 `f8904e4b04bca3b47bc52a683f6190365d2eb89ee8f44f8072759e9c5e04a553`.
The reader found the two character pointers through the previously identified address `0x14470de90`.
Their character kinds were `0` and `1`.

Each sample records both character structures and the global fields needed by the timing function.
The reader captures the lookup structures that the movement function can inspect.
Each of the two captures with those structures contains 32 samples, which provide 64 fighter states per capture.
The second capture samples the game approximately every 600 milliseconds for about 20 seconds.

These reads are asynchronous, which means they occur independently of the game's update steps.
The game can change memory between reads, so a sample may combine fields from different moments.
The capture does not establish the state at the movement function's entry or its output at return.

## Comparison outside the running game

The comparison executes the original movement and timing instructions through Rosetta 2.
The other program executes the corresponding C++ reconstruction as native ARM64 code.
Both programs receive the captured values.

The programs copy the characters into their own memory.
They adjust the opponent pointer to reference the copied opponent.
They adjust the lookup pointer to reference its captured header.
They preserve the numerical inputs and the lookup's captured entry count.
These pointer adjustments allow both programs to use the copies at different addresses.

The captured lookup structures contain zero entries.
Therefore, the original function skips the branch that would call the lookup function and inspect other battle objects.
The replay rejects a nonempty lookup structure or a missing lookup header.
It continues to reject the unsupported rotation path.

For each fighter state, both programs restore the input and execute one movement call.
They compare the same 72 bytes used in the [constructed-state experiment](native-physics-proof.md).
Both programs verify that all character bytes outside those observed ranges remain unchanged.

The captures produced these results:

| Capture | Fighter states | Output bytes | Calls that changed position | Mismatching states |
| --- | --- | --- | --- | --- |
| Initial idle capture | 64 | 4,608 | 0 | 0 |
| Movement capture | 64 | 4,608 | 5 | 0 |

Both programs completed every state, and all output bytes matched.
The second capture contains changing fighter positions, but all recorded motion codes are `0` and all recorded heights are zero.
Therefore, it does not validate jumps or airborne movement using captured inputs.

This establishes agreement for the selected captured inputs.
It does not establish movement during an actual battle because the comparison executes outside the game.
FEX supplies the captured inputs, while Rosetta 2 executes the original reference instructions during the comparison.

## Private artifacts

The reader, captures, recovered source, and comparison programs remain outside Git:

`/Users/iphoobis/Library/Application Support/SC6-VRR/native-investigation/live-capture/`

The first capture that includes lookup structures is `training-20261016-174521/`.
The movement capture is `training-20261016-174720/`.
Its `capture.json` records how the capture ran.
Its `replay-result.json` records the successful comparison.

`capture.py` checks the original file before running the reader.
`run_replay.py` checks the file and capture metadata before running the two comparison programs.

The [recorder for movement calls](native-boundary-recorder.md) has now captured inputs and outputs around 64 actual calls in SC6.
The ARM64 reconstruction and original x64 instructions each match all recorded output bytes for those calls.
Airborne inputs remain unverified.
General replacement of the function still requires recovery of its unsupported branches.
Rendering, HDR, 120 Hz interpolation, and sustained performance remain unverified.
