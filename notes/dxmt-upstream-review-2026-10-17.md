# DXMT patch review for SC6

Checked on 2026-10-17. The review covers all 7 open pull requests, including the draft, and all 21 open issues.
I screened the titles of all 51 closed issues for relevant fixes. That screening was not a full review of every closed discussion.

The user subsequently requested integration of 204, 230, 228 and 222.
That integration is complete in the [integration report](dxmt-integration-2026-10-17.md).
The version and patch-state statements below describe the earlier review snapshot.

## Recommended order

1. Measure whether SC6 triggers the expensive buffer-update path described in [issue 204](https://github.com/3Shain/dxmt/issues/204).
2. Test the timestamp reproducer from [issue 230](https://github.com/3Shain/dxmt/issues/230) separately from performance measurements.
3. Record the formats of buffer views before considering the correction described in [issue 228](https://github.com/3Shain/dxmt/issues/228).
4. Consider [PR 222](https://github.com/3Shain/dxmt/pull/222) only when expanding compatibility with other applications.

A constant buffer holds values that shaders read. Issue 204 changes how repeated updates to these buffers obtain memory.
A timestamp query measures when GPU work runs. Issue 230 fixes result availability after a later completion event.

## Build versions

- The installed build reports `v0.80-273-g1029cba`.
- The source checkout is `5876db4`. It adds a geometry-conversion change that the installed build does not contain.
- The upstream base shared with the custom branch is `186a232`.
- Current upstream `main` is `50102f9`. That one newer commit fixes `nvngx` resource lookup.
- No patch from this review has been applied. The source checkout remains unchanged.

The compiled binary and the source checkout must be tracked separately. A recent source checkout does not make an older binary current.

## Open pull requests

| PR | Recommendation | Evidence and limits |
| --- | --- | --- |
| [#222](https://github.com/3Shain/dxmt/pull/222) | Optional general compatibility | This is the preferred Direct2D implementation. It supersedes PRs 171 and 172. It applies cleanly to the custom source. User reports show a launcher no longer crashes, but some text and progress elements remain missing. No SC6 performance gain is established. Head: `3a9a303379ea`. |
| [#217](https://github.com/3Shain/dxmt/pull/217) | Defer | This draft adds generated D3D12 interface definitions and changes the build. It does not improve the current SC6 D3D11 path. Its full patch does not apply cleanly. Head: `0adc0a0b0089`. |
| [#184](https://github.com/3Shain/dxmt/pull/184) | Defer as-is | This preserves a shared Metal view while overlapping swapchains exist. It applies cleanly, but acquire and final release can race because creation and destruction happen outside the map lock. It is not a fix for direct presentation. Test overlapping lifetimes and concurrent creation/destruction before adoption. Head: `1a8b59cee690`. |
| [#172](https://github.com/3Shain/dxmt/pull/172) | Superseded | PR 222 provides the replacement. Do not stack this implementation with PR 222. Head: `dd8c2d1993b5`. |
| [#171](https://github.com/3Shain/dxmt/pull/171) | Superseded | PR 222 provides the replacement. The full patch does not apply cleanly to the custom source. Head: `20bf2160c020`. |
| [#166](https://github.com/3Shain/dxmt/pull/166) | Defer until Wine support is verified | This changes how a swapchain obtains the Metal layer through ExtEscape. It requires the Wine change linked as MR 11058. It applies cleanly, but changes the same creation/destruction sites as PR 184. No current SC6 benefit is demonstrated. Head: `a1aa73fd8a85`. |
| [#133](https://github.com/3Shain/dxmt/pull/133) | Defer for the Wine build | This adds SDL2 and SDL3 window backends for native builds. Maintainer changes remain requested. It is not the presentation path used by SC6 under Wine. Its full patch does not apply cleanly. Head: `eb567a3c515a`. |

## Open issues

| Issue | Recommendation | Evidence and limits |
| --- | --- | --- |
| [#230](https://github.com/3Shain/dxmt/issues/230) | Candidate for a separate correctness test | A patch and a standalone reproducer are available. It makes timestamp results ready after a later event completes. SC6 uses UE4, but its measured waits are event queries. No SC6 benefit is demonstrated. The patch adds a CPU wait and needs review of concurrent access to command chunks. |
| [#229](https://github.com/3Shain/dxmt/issues/229) | Defer for SC6 | A 52,601-byte patch combines NV12 video textures and a desktop-window swapchain workaround. Split those changes before evaluation. No corresponding SC6 failure has been observed. |
| [#228](https://github.com/3Shain/dxmt/issues/228) | Investigate if SC6 uses affected buffer views | The report describes a correction and provides a reproducer, but it does not publish its patch. Affected formats hold three 32-bit components per buffer element. Existing SC6 logs show buffers without their view formats, so they cannot establish whether SC6 uses this case. |
| [#204](https://github.com/3Shain/dxmt/issues/204) | Highest-priority performance experiment | The report describes a small allocation change and supplies a benchmark. It reduces separate Metal buffer allocations for repeated DEFAULT buffer updates. The local constructor still uses the reported path. Measure SC6 update counts first. Recorded command lists can restore an allocation cursor, so that path needs regression coverage. |
| [#200](https://github.com/3Shain/dxmt/issues/200) | Do not reapply the stall fix | The fatal loop bug and zero-count handling are already fixed in aadf0fb and 46911d7. The installed 1029cba build contains those commits. Remaining overflow and malformed-input cases are hardening work, not a demonstrated SC6 fix. |
| [#183](https://github.com/3Shain/dxmt/issues/183) | Track through PR 184 | This reports overlapping ANGLE surfaces that detach the shared Metal view. SC6 has no matching white-window failure in the current test. |
| [#161](https://github.com/3Shain/dxmt/issues/161) | Defer | This concerns 32-bit processes and placed buffers on Tahoe. SC6 runs as a 64-bit process in the Rosetta bottle. The report does not supply a ready DXMT fix for this setup. |
| [#152](https://github.com/3Shain/dxmt/issues/152) | No runtime patch | This discusses licensing. It is not a graphics or performance patch. |
| [#151](https://github.com/3Shain/dxmt/issues/151) | Track planned features | This is the 1.0 roadmap. It links stream output, null binding, and cross-process rendering, but it is not a patch. |
| [#141](https://github.com/3Shain/dxmt/issues/141) | Defer for SC6 | Comments provide Wine patches for remote Metal layers used by Steam CEF. Later tests report corrected resize gaps and Retina placement. This requires Wine changes and has no demonstrated benefit for the current single-process SC6 presentation path. |
| [#135](https://github.com/3Shain/dxmt/issues/135) | Defer until a video failure | The issue links Proton Media Foundation patches. Follow-up comments report a leak and further required changes. No corresponding SC6 cutscene failure has been observed. |
| [#99](https://github.com/3Shain/dxmt/issues/99) | Track; no ready patch | This requests safe dummy resources for null bindings. No patch or SC6 failure is supplied. |
| [#96](https://github.com/3Shain/dxmt/issues/96) | Track; no ready patch | This concerns an assumed limit on tessellation fork/join instances. No affected SC6 call or ready patch is shown. |
| [#95](https://github.com/3Shain/dxmt/issues/95) | Track; no ready patch | This reports oversized output from mesh shaders used for tessellation. No ready patch is supplied, and the report concerns another GPU generation. |
| [#79](https://github.com/3Shain/dxmt/issues/79) | Reference only | ANGLE conformance results help diagnose ANGLE. They do not supply an SC6 patch. |
| [#72](https://github.com/3Shain/dxmt/issues/72) | Defer | An old patch and traces concern a 32-bit D3D10 sample and its sample count. The follow-up still reports incorrect rendering. It is not a validated 64-bit D3D11 SC6 fix. |
| [#71](https://github.com/3Shain/dxmt/issues/71) | Defer | This requests DLSS frame generation and ray reconstruction. It provides no patch. It does not implement interpolation for SC6 battle state. |
| [#37](https://github.com/3Shain/dxmt/issues/37) | Track; no ready patch | This reports a mismatch between a shader output type and the render target format. No matching SC6 failure or ready patch is supplied. |
| [#28](https://github.com/3Shain/dxmt/issues/28) | Track; no ready patch | Geometry-shader stream output is a planned feature. This issue contains no patch. Ordinary geometry shaders and stream output are separate features. |
| [#26](https://github.com/3Shain/dxmt/issues/26) | Relevant limitation; no ready patch | This reports disagreement over the display refresh rate used to interpret SyncInterval. It matters for ProMotion, but it supplies no patch. The current launcher retains the existing VSync-off setting. |
| [#25](https://github.com/3Shain/dxmt/issues/25) | No runtime patch | This is a housekeeping agenda about style and formatting. |

## PR 184: possible lifetime conflict

Static review indicates a possible unsafe order between two threads. This has not been reproduced in a runtime test.

1. The final release removes the view from the shared map, then unlocks the map.
2. A new acquire can receive the same existing view from Wine before that release finishes.
3. The acquire can insert that view into the map again.
4. The old release can then detach the view that the new swapchain still uses.

The patch therefore needs a test that overlaps final release with a new acquire.
A clean patch-fit check does not address this behavior.

## Validation and limits

The review checked the downloaded diffs against the current custom source with `git apply --check`.
That command checks whether a patch fits. It does not compile the patch or establish correct runtime behavior.
PRs 166, 172, 184, and 222 pass that check. PRs 133, 171, and 217 do not.
The timestamp patch from issue 230 passes the same check.

The fatal fix from issue 200 is already an ancestor of the installed build.
Current SC6 measurements identify event-query waits, so issue 230 is not a demonstrated cure for the measured frame time.
Existing frame logs do not record DEFAULT buffer update counts or the formats of buffer views.
Those missing measurements prevent a justified SC6 speedup claim for issues 204 and 228.

Local evidence is in `tests/runs/upstream-review-20261017/`.
It contains API snapshots, PR diffs, issue comments, patch-fit checks, and `review.json`.
