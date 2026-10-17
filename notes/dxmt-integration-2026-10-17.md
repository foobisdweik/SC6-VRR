# DXMT integration for CrossOver Preview

## Installed build

The custom source is `/Users/iphoobis/Projects/dxmt-SC6`.
The branch is `codex/sc6-204-230-228-222`.
The installed commit is `000e282f4f79597a609b95407c808590331a17d9`.
The build reports `v0.80-283-g000e282`.

This build retains native fullscreen, the BGR10A2 setting, and the existing geometry-conversion change.
It includes upstream `main` through `50102f994b406f58f56d5d09f2ad20f9b90390a2`.
That upstream commit corrects `nvngx` resource lookup.

The source changes are local experiments. They have not been submitted upstream.

## Integrated changes

| Source | Implementation | Validation |
| --- | --- | --- |
| [Issue 204](https://github.com/3Shain/dxmt/issues/204) | Cherry-picked `9434028` as `f67f434`. Small buffer updates share pages. A separate correction prevents immediate updates from overwriting memory after command-list replay. | 255 replays with intervening updates preserve every expected value. This tests correctness, not an SC6 performance gain. |
| [Issue 230](https://github.com/3Shain/dxmt/issues/230) | Adapted the published timestamp patch. It records the chunk and the next GPU event. It uses existing monotonic events instead of reading a command buffer while another thread may release it. | 200 reused timestamps remain nonblocking before submission and become available after a later event. The original installed build failed all ten calibration attempts. |
| [Issue 228](https://github.com/3Shain/dxmt/issues/228) | Implemented the described RGB32 correction locally. Physical offsets and indices use three R32 texels per element. The shader gathers three components while lengths remain in logical elements. | FLOAT, UINT and SINT views pass at three starting offsets. Swizzles and bounds pass. R32, RG32 and RGBA32 controls pass. The author's original FLOAT reproducer failed before integration. |
| [PR 222](https://github.com/3Shain/dxmt/pull/222) | Cherry-picked both `3a9a303` and `b1e4c30`. Corrected staging surface descriptions, subresource bounds, null arguments and ownership of separately created surfaces. | State swaps restore both states. Surface mapping preserves contents. Repeated subresource surfaces release their parent. Direct2D draws red into a DXMT surface, and D3D11 reads the expected pixel. |

Timestamp results are published by the finish thread after its chunk completes.
Once a later GPU event has signalled, a timestamp read may wait for remaining work in that submitted chunk.
A timestamp read does not wait for a chunk that has not been submitted.

The shader cache version is `(AIRCONV_VERSION << 16) | 1`.
This separates the new RGB32 metadata from earlier compiled shaders.
Without that separation, an old cached shader can hide the correction.
No existing shader cache was deleted.

The built-in HLSL compiler rejects `Buffer.GetDimensions` in these test shaders.
Therefore, the runtime tests do not validate buffer dimension queries.
The converter masks the RGB32 flag when reading the logical element count.

## Installation

The installation replaced these files under Preview's `Contents/SharedSupport/CrossOver/lib/dxmt`:

- `x86_64-windows/d3d10core.dll`
- `x86_64-windows/d3d11.dll`
- `x86_64-windows/dxgi.dll`
- `x86_64-windows/winemetal.dll`
- `x86_64-windows/nvapi64.dll`
- `x86_64-windows/nvngx.dll`
- `x86_64-unix/winemetal.so`

The rebuilt `winemetal.dll` has the same bytes as the preceding build.
The other six files changed.
Hashes confirm that the i386 and aarch64 libraries stayed unchanged.
These x86-64 replacements affect other x86-64 DXMT games in Preview.

A complete backup of the seven preceding files is here:

```text
/Users/iphoobis/Library/Application Support/SC6-VRR/backups/preview-before-204-230-228-222-20261017
```

Its `SHA256SUMS` matches the hashes recorded before integration.
The staged runtime contains the installed files, their hashes, the source commit and the build version:

```text
/Users/iphoobis/Library/Application Support/SC6-VRR/runtimes/dxmt-direct-x86_64
```

## Checks

`ninja -C build -j 6` completed successfully.
The compiler still reports existing missing-return warnings in airconv.
The source checkout is clean.
The Windows files are x86-64 PE DLLs. The Unix library is an x86-64 Mach-O library.
`otool -L` shows system runtime dependencies and the expected Wine libraries.

`tests/check-dxmt-patches.sh` passes all 27 checks through Preview's `SC6-Rosetta` bottle.
The shader cache remains enabled in the final test.
`tests/check-direct-dxmt.sh` creates a device and finds the custom fullscreen entry point.
Its null-window result establishes that the entry point loaded. It does not establish direct presentation for a game window.

The evidence is in `tests/runs/dxmt-integration-20261017/`.
The final regression output is in `final/regression.log`.
SC6 performance and full battle behavior with this graphics build remain unmeasured.
Game Mode activation remains unresolved.

## SC6 launch check

On 2026-10-17 at 15:47, `dll/launch.sh` started SC6 in `SC6-Rosetta` with this build installed.
`lsof` on the game process listed Preview's `x86_64-unix/winemetal.so` and the x86-64 `d3d11.dll`, `dxgi.dll` and `winemetal.dll`.
The installed files had the same SHA-256 hashes as the staged runtime, so the game loaded this build.
The game ran for about three and a half minutes. It exited at about 15:51:28.
Neither Wine nor macOS logged a crash, and no crash report was written. Whether the user quit the game is unconfirmed.
`sc6vrr.log` reported about 76 engine frames and 60 scheduler steps per second at 15:50.
`wine-unlocked.log` had no DXMT errors. It had two GStreamer video assertions while the intro movies played.
The shader cache file name stays `shaders_320.db`. `dxmt_shader_cache.cpp` passes the new cache version to the cache reader and writer for that file. This check did not confirm that the first launch recompiled every shader.
The user then confirmed that the picture looked correct.
The presentation indicator showed `Direct`, with a split-second `Composited` flash every 3–4 seconds.
The preceding build showed the same flashes, so this build's changes probably did not cause them.
This check did not measure a battle.
