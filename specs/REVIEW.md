# Specification review log

Each entry records one check of a specification against the rules in [README.md](README.md), before release to the implementation side.

## R-001 Renderer frame structure, draft 1 (2026-10-17)

Sources: two one-frame logs from the analysis DLL (frames A and B, see the specification). The logs record Direct3D 11 calls at the API boundary: bindings, clears, viewports, dispatches, and draws. No disassembly or decompiler output was used. Role names come from the public structure of UE4's deferred renderer and are marked inferred.

| Check | Method | Result |
| --- | --- | --- |
| No code, pseudocode, or disassembly | Searched for code blocks and decompiler terms (`param_`, `FUN_`, `DAT_`, `undefined`) | None found |
| No addresses or object layouts | Searched for `0x` numbers and hex strings of 12 or more digits | Only sizes such as "640x360" matched; no addresses |
| Roles, not capture names | Searched for the frame log's texture names (`T` plus a number) | None found |
| Shader identities | Read the specification | Pixel shader counts only; no shader hashes |
| Observed and inferred are marked | Read the specification | Marked throughout: 21 explicit labels, and role columns are labeled inferred |
| Stage and target facts match the logs | Recomputed each target's writing and reading stages from frame B with `tests/frame_passes.py` | The targets table was corrected to match before this entry |

Outcome: passes the rules check. Not yet released, because the "Not covered yet" items leave it incomplete for implementation.
Reviewer: Claude (analysis-side session). The user approves releases.

## R-001 Renderer frame structure, draft 2 (2026-10-17)

New source: frame C, a one-frame log that also records depth-stencil, rasterizer, and sampler state descriptions and vertex shader texture bindings. As before, only API-boundary observations were used.

| Check | Method | Result |
| --- | --- | --- |
| No code, pseudocode, or disassembly | Searched for code blocks and decompiler terms | None found |
| No addresses, hashes, or capture names | Searched for `0x` numbers, hex strings, 8-digit hash-like IDs, and `T` plus a number | None found |
| State tables match the logs | Compared each depth-stencil and rasterizer row with the stored descriptions and the passes that bind them in frame C | Match |
| Clear values match the logs | Compared each clear row with frame C's clear calls and their targets | Match |
| Observed and inferred are marked | Read the new sections | Marked; purposes and roles are labeled inferred |

Outcome: passes the rules check. Still not released: constant data, per-pass slot lists, and shader lookup are missing.
Reviewer: Claude (analysis-side session). The user approves releases.
