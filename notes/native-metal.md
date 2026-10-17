# Metal rendering plan for the native program

Recorded on 2026-10-16. This is a plan. No step below has run yet.

The game already draws through Metal today, but only through a translation layer (DXMT or D3DMetal) under CrossOver.
This plan covers the native program from [the clean-room plan](native-port.md), which needs its own Metal renderer.
HDR, ProMotion, and interpolation stay deferred. The first renderer targets SDR output at the original 60 Hz.

## Installed tools (checked 2026-10-16)

| Tool | Version | Role here |
| --- | --- | --- |
| `metal` (Metal Toolchain 27.2) | `32023.922`, target `air64-apple-darwin27.2.0` | Compiles Metal Shading Language (MSL) to AIR (Apple's intermediate shader format) and metal libraries. |
| `metal-shaderconverter` | `4.0.1` | Converts DXIL (the shader format of Direct3D 12) to a metal library. This meets the target of 4.0 beta 2 or newer. |
| `dxc` | `1.9.0.5512` | Compiles HLSL source. Its `-metal` option can produce Metal output. It cannot read compiled game shaders. |
| `spirv-cross` and SPIR-V tools | Homebrew | Convert SPIR-V (a portable shader format) to MSL. |
| DXMT `airconv` | fork at `~/Projects/dxmt-SC6` | Converts DXBC (the shader format of Direct3D 11) directly to AIR. It already converts SC6's shaders on this Mac. |

SC6 uses Direct3D 11 with DXBC shaders ([dxmt.md](dxmt.md)).
Metal Shader Converter accepts DXIL, not DXBC. So that path needs a DXBC-to-DXIL step, and no such tool is installed.

## Where each part stands in the clean room

- Frame captures and shader logs from the running game are analysis material. They stay private.
- The game's own shaders may be converted on the user's Mac. The converted shaders stay local, like any other converted data.
- The renderer (pass order, render targets, state setup, and draw submission) is new code written from specifications.
- New shaders that the renderer needs, for example for presentation, are written new in MSL or HLSL.

The shipped program must read shader bytecode from the user's pak files.
That depends on the open pak decryption question in [the legal section](native-port.md#pak-encryption-open-blocks-release).
Steps 1 to 5 avoid that question, because they use runtime captures instead of pak files.

## Steps

1. **Log every shader that the game creates.** Extend the existing `dinput8.dll` hook to record each `ID3D11Device::Create*Shader` call: stage, size, hash, and shader model. Save the bytecode to the private investigation directory. This gives the real count and format without decrypting pak files.
2. **Capture one battle frame through DXMT.** Use a Metal GPU capture of DXMT's output in Training mode. This shows the pass order, render target formats, resolutions, draw counts, and resource bindings that the native renderer must reproduce. The capture method under CrossOver is unverified.
3. **Test shader conversion on the full logged set.** Compare two paths by success rate and compile time:
   1. DXBC to AIR through `airconv`, then a metal library through the Metal Toolchain. DXMT is LGPL-2.1, so the shipped program could use it as a separate, replaceable library.
   2. DXBC to DXIL, then `metal-shaderconverter`. Microsoft's `dxilconv` (in the DirectXShaderCompiler repository) converts DXBC to DXIL. Whether it builds on macOS is unverified.
4. **Write the renderer specifications.** The analysis side describes the passes, formats, states, and binding rules from step 2. The rules in [Clean-room process](native-port.md#clean-room-process) apply.
5. **Build the Metal host and replay a captured frame.** The implementation side writes a host app with `CAMetalLayer`. It replays the captured draw stream (private test data) through the converted shaders. Compare its image with the DXMT capture. This tests the renderer before asset loading exists.
6. **Load assets from the installation.** Draw one character from the user's files. This step waits on the pak decryption question for release, but not for private testing.

## Step 1 status (2026-10-16)

The `dinput8.dll` hook now records shaders when `SC6VRR_SHADER_DIR` is set:

1. It hooks the `D3D11CreateDevice` and `D3D11CreateDeviceAndSwapChain` exports of `d3d11.dll`, which the exe imports.
2. When the game creates its first device, it replaces the seven `Create*Shader` pointers in the device's vtable (slots 12 to 18). It does not patch the methods' code. In the FEX bottle, `d3d11.dll` is ARM64EC, so the first build's x64 code patch crashed the game with an illegal instruction (`0xC000001D` at `d3d11.dll+0x51bc`, the pixel-shader method).
3. Each unique blob goes to `dxbc/<sha256>.dxbc`. Each call adds a line with stage, shader model, size, hash, and result code to `index-<time>-<pid>.tsv`.

Frame changes require `SC6VRR_FRAME_MODS=1`. The cases that request frame changes now set that variable.
For unlocked rendering with game logic at 60 Hz, use `dll/launch.sh`.
For shader capture with the same timing, use `tests/shader-capture.sh launch-unlocked`.
The old `launch-fast` name is an alias for that command. It now preserves the normal game speed.
See [the timing implementation](unlocked-rendering.md).

`tests/test_shader_capture.c` checks the parser, the hash, deduplication, the passed-through result codes, and the stored bytes with a fake device. It passes in the CrossOver Preview `SC6-fex` bottle.

The second in-game run (2026-10-16, 18:46 to about 18:48) completed without a crash:

| Measure | Result |
| --- | --- |
| Create calls | 383, with 0 failed calls |
| Unique shaders | 373, all stored, 1.8 MB of bytecode |
| Format | All DXBC shader model 5.0 |
| By stage | 276 pixel, 92 vertex, 4 compute, 1 geometry, 0 hull or domain |
| Timing | Calls arrived in bursts at about 0, 30, and 60 to 79 seconds after the first call, and they stopped 80 seconds after it |

The game ran for only about two minutes. DXMT last wrote its shader cache at 18:48:12, which probably marks the exit.
So this set covers the scenes visited in that time, not the whole game.
Whether every creation path passes through the hooked vtable is unverified. A longer session that keeps loading new stages and characters would test that: new loads must keep adding new shaders.

### FEX and Rosetta comparison (2026-10-16)

The user repeated the run in `SC6-Rosetta` (CrossOver Preview, x86-64, Rosetta 2) with the same stage and two characters. The attacks differed from the FEX run.

| Measure | FEX | Rosetta |
| --- | --- | --- |
| Unique shaders | 373 | 550 |
| Create calls | 383, 0 failed | 590, 0 failed |
| Time from first to last call | 80 s | 132 s |
| Crash | none | none |

- 372 shaders appear in both runs, and their bytes are identical.
- 1 pixel shader appears only in the FEX run. 178 shaders (140 pixel, 34 vertex, 4 compute) appear only in the Rosetta run.
- The Rosetta run kept adding new shaders until its end, in bursts at about 45, 75, and 105 seconds.

The shaders come from the game files, so both bottles create the same bytecode for the same content.
The differences in the sets probably come from different attacks and the longer session, not from the emulator.
The order of first creation matches only for the first 17 shared shaders. The game probably loads on several threads, so that order is not meaningful.
For shader work, either bottle is a valid source. The union of both runs has 551 unique shaders.

From 2026-10-16, all runs use only `SC6-Rosetta`. The scripts default to it, and the user removed the game from `SC6-fex`.

### Long Rosetta session (2026-10-16 22:38 to 2026-10-17 01:15)

A play session of about 2.5 hours in `SC6-Rosetta` completed without a crash.
Together with the earlier Rosetta run, the bottle's set now holds 2,038 unique shaders (9.7 MB) from 13,379 create calls, with 0 failed calls.
By stage: 1,769 pixel, 259 vertex, 9 compute, 1 geometry. All are DXBC shader model 5.0.

New shaders slowed sharply over the session. The first 1,000 calls added 387 new shaders, but each of the last 4,000 calls added only about 2 to 10 new shaders per 1,000 calls.
So the set is probably close to complete for the content played. Content that was not played, such as other modes, can still add shaders.
New shaders kept arriving late in the session, which supports the hook catching later loads.

`tests/shader-capture.sh` installs the DLL into `SC6-fex`, launches the game with capture on, summarizes the results, and removes the DLL. Output goes to `~/Library/Application Support/SC6-VRR/native-investigation/shaders/<bottle>/`. `SC6_BOTTLE=SC6-Rosetta` selects the CrossOver Preview x86-64 bottle, which runs the game under Rosetta 2. Both DLL tests also pass there.

## Step 3 status, path 1: `airconv` (2026-10-17)

`tests/convert-shaders.sh` converts each captured shader with DXMT's `airconv` (fork `dxmt-SC6` at `1029cba`, native macOS build) and links the result with `metallib` from Metal Toolchain 27.2.
`tests/load-metallibs.swift` then loads each library with the Metal runtime on the M1 Pro and compiles what it can to GPU code.

| Stage | Shaders | Converted to AIR | Linked | Loaded by Metal | Compiled to GPU code |
| --- | --- | --- | --- | --- | --- |
| Pixel | 1,769 | 1,769 | 1,769 | 1,769 | not tested |
| Vertex | 259 | 259 | 259 | 259 | 259 |
| Compute | 9 | 9 | 9 | 9 | 9 |
| Geometry | 1 | 0 alone | 0 | — | — |
| All | 2,038 | 2,037 | 2,037 | 2,037 | 268 of 268 tested |

- The conversion of all 2,038 shaders took 23 seconds with 8 parallel jobs.
- The geometry shader fails alone, because `airconv` converts a geometry shader only together with its vertex shader (`--vertex-before-geometry`). Paired with captured vertex shaders, it converts. The capture does not show which vertex shader the game really pairs with it.
- Vertex shaders compiled in depth-only pipelines with a generated vertex buffer layout. The game's real layouts differ, so this proves compilation, not correct vertex input.
- Pixel shaders were not compiled to GPU code. Metal compiles a pixel shader only with a vertex shader whose outputs match its inputs, and the capture does not record which shaders the game uses together.

To close the gaps, the DLL should also record the shader pairs and input layouts that the game binds for each draw. Step 2 (the frame capture) needs that information anyway.

### Pipeline-state recording (added 2026-10-17, not yet run in the game)

The first capture recorded only shaders. That was a planning gap: compiling pixel shaders needs the real pipelines too. So the 2.5-hour session must be repeated once.
The DLL now records, for each new combination that a draw uses, everything a Metal render pipeline depends on:

| Recorded | Where | Why Metal needs it |
| --- | --- | --- |
| Vertex, geometry, and pixel shader | `bindings-*.tsv` (SHA-256 of each) | A pipeline links one vertex function with one fragment function. |
| Input layout | `layouts/<sha256>.txt` | It becomes the Metal vertex descriptor. |
| Vertex buffer strides | `bindings-*.tsv`, only for slots the layout reads | The vertex descriptor needs each buffer's stride. |
| Primitive topology | `bindings-*.tsv` | Metal pipelines declare the primitive class. |
| Render target and depth formats, multisampling | `bindings-*.tsv` (numeric `DXGI_FORMAT` values) | A pipeline is compiled for specific attachment formats. |
| Blend state | `blend/<sha256>.txt` | Metal builds blending into the pipeline. |

Textures, samplers, constants, and depth-stencil states are not recorded. They do not change pipeline compilation, and step 2's frame capture covers them.
The hooks swap vtable pointers on the device and the immediate context. Vtable slot numbers come from `d3d11.h` with `offsetof`, and `_Static_assert` checks them against the documented order.
The fake-device test passes in `SC6-Rosetta`. It covers deduplication, unused stride slots, kept render targets, and `none` and `unknown` objects.

### Fast-mode session (2026-10-17, 01:50 to 02:11)

The user ran CPU-versus-CPU battles with `tests/shader-capture.sh launch-fast` at maximum settings.
In that earlier run, fast mode omitted the 60 Hz gate, so battle logic advanced at about 190 to 240 Hz.
The current launcher always enables the 60 Hz world gate when it unlocks rendering.

Across all `SC6-Rosetta` runs after this session:

| Measure | Result |
| --- | --- |
| Unique shaders | 2,246 (1,934 pixel, 295 vertex, 9 compute, 8 geometry), 0 failed creates |
| Input layouts, blend states | 30, 20 |
| Unique pipeline combinations | 1,748 |
| Pixel shaders seen in a real pair | 1,159 of 1,934 (60%) |
| Vertex shaders seen in a real pair | 201 of 295 |
| Objects created before the hooks | 0 |

Maximum settings added 208 shaders that the earlier sessions had not created, including 7 geometry shaders.
The game recreates equal objects at new addresses, so this run saw 32,046 address combinations for those pipelines. The recorder now keys pipelines by content IDs (a small number per unique SHA-256), so equal content is one entry and the tables cannot fill during long sessions.
The 40% of pixel shaders without a recorded pair probably belong to content not shown in these battles, or to the earlier low-settings session. This is unverified.

### Real pipelines compiled with Metal (2026-10-17)

`tests/compile-pipelines.swift` rebuilds each recorded combination as a Metal render pipeline and compiles it on the M1 Pro. It uses the converted vertex and pixel shaders, a vertex descriptor from the input layout and strides, the recorded target and depth formats, the blend state, and the primitive class.

| Result | Pipelines |
| --- | --- |
| Recorded combinations | 1,748 |
| With a vertex and a pixel shader | 1,693 |
| Compiled | 1,652 |
| Failed | 0 |
| Skipped: they include a geometry shader | 41 |

- 1,140 of the 1,159 pixel shaders that were seen in pairs compiled in at least one real pipeline. The other 19 appear only in geometry-shader pipelines.
- The whole run took 20 seconds.
- Mappings that the test assumes: vertex buffers use Metal buffer indices 30 minus the D3D slot, and D3D's 24-bit depth format becomes `depth32Float_stencil8`. Metal reported no conflict with either.
- Stride 0 vertex buffers (every vertex reads the same element) become a constant step with a nonzero stride value, because Metal asserts on a zero stride.
- This proves compilation, not correct output. DXMT's real runtime pulls vertices in the shader instead of using a Metal vertex descriptor, so its pipelines differ from these.

### Geometry-shader pipelines (2026-10-17)

DXMT runs a geometry shader as a Metal mesh pipeline. The vertex shader, converted together with the geometry shader, is the object function. The geometry shader is the mesh function, and the pixel shader (if any) is the fragment function.

1. The first attempt converted each pair without an input layout. All 51 mesh pipelines then crashed Apple's GPU compiler (`MTLCompilerService`, `EXC_BAD_ACCESS` in `AGXCompilerCore` while compiling the object program).
2. DXMT's runtime always passes the input layout, the index buffer format, and a strip-topology flag to this conversion (`d3d11_shader.cpp`). The `airconv` command line had no way to pass them.
3. The local `dxmt-SC6` branch `sc6/airconv-cli-gs` adds `--ia-layout`, `--index-format`, and `--strip-topology` to `airconv_cli.cpp`. It is uncommitted and stays local.
4. `tests/gs_variants.py` converts each recorded combination with its real input layout (elements mapped to registers through the vertex shader's input signature) and its strip flag.

With those arguments, all 33 variants convert, and all 51 recorded mesh pipelines compile, 10 of them depth-only. The compiler did not crash again.
The capture does not record the index buffer format, so every variant assumes 16-bit indices.

| Step 3 totals | Result |
| --- | --- |
| Recorded pipelines tested | 1,703 (1,652 ordinary, 51 mesh) |
| Compiled | 1,703 |
| Failed | 0 |
| Pixel shaders seen in pairs that compiled in a real pipeline | 1,159 of 1,159 |

`airconv` aborts when it converts a geometry shader alone, which opens a macOS crash dialog, so `tests/convert-shaders.sh` no longer tries that.

Still open in step 3: the 775 pixel shaders without a recorded pair, which more play would cover, and step 3 path 2 (Metal Shader Converter).

Path 2 (DXBC to DXIL, then Metal Shader Converter) has not been tested.

## Step 2 setup: one-frame capture (2026-10-17, not yet run in the game)

`tests/shader-capture.sh launch-frame` starts SC6 at normal speed with two captures armed. Pressing F10 in a battle triggers both:

1. **The DLL's frame log.** It hooks `IDXGISwapChain::Present` through `CreateDXGIFactory`/`CreateDXGIFactory1` and the swap chain's vtable. The key press arms the log at one Present, and the log covers the next full frame up to the following Present. It records, in call order: render target and depth bindings (texture address, size, format, mips, array size, samples), shader resource and unordered access view bindings, clears, viewports, copies, resolves, compute dispatches, and each draw with its counts and pipeline (16-digit hash prefixes, matching the shader and binding files). The output goes to `frames/frame-*.tsv`.
2. **DXMT's Metal capture.** `MTL_CAPTURE_ENABLED=1` and `DXMT_CAPTURE_EXECUTABLE=SoulcaliburVI` make the DXMT in CrossOver Preview capture the next frame to a `.gputrace` document that Xcode opens. `tests/shader-capture.sh frames` moves it into the private `frames/` folder. The capture is probably written to the game's working folder; this is unverified.

Both react to the same key at the next frame, so they describe the same frame or adjacent ones. Texture addresses identify resources only within one frame log.
The fake-device test covers the key edge, the frame boundaries, released resource references, and the logged lines.

### Frame log additions for the specification gaps (2026-10-17, not yet run in the game)

The DLL now also records depth-stencil, rasterizer, and sampler states. Each unique description goes to `depth/`, `raster/`, or `sampler/` (index stage codes `dss`, `rss`, `sss`). Metal keeps these states outside the render pipeline, so they do not change the recorded pipeline combinations.
In the frame log, each draw now names its depth-stencil state, stencil reference value, and rasterizer state. The log also lists vertex shader texture bindings and the samplers bound for pixel, vertex, and compute shaders.
Clear lines already named their target, so `tests/frame_passes.py` now prints each clear with its target and value, plus each pass's states and sampler count.
Constant data is still not recorded.

### First frame capture (2026-10-17, Training)

Both captures worked. The DLL wrote a 2,912-call frame log, and DXMT wrote `SoulcaliburVI_F.2458_*.gputrace` (frame 2458). Both are in the private `frames/` folder. `tests/frame_passes.py` splits a frame log into passes, where a pass starts whenever the render or depth targets change.

The frame has 94 passes with work, 982 draws, and 4 compute dispatches. It reads 404 textures, most of them block-compressed (BC1, BC3, BC5) material textures.
The game rendered at 640x360, the resolution in the user's settings (`ResolutionSizeX=640`). The 200% resolution scale in the settings did not enlarge the render targets; the reason is unverified.

The pass names below are my reading, based on UE4 4.17's deferred renderer. They match the targets, formats, and dependencies, but they are not confirmed:

| Passes | Probable stage | Evidence |
| --- | --- | --- |
| 1 | Depth prepass | 277 draws into depth only (`T4`, 640x360, 24-bit depth with stencil) |
| 2 | Hierarchical depth buffer | 512x256 `R16F` with 9 mips, reads the depth |
| 3 | Shadow depths | 95 draws into a 4096x2048 16-bit depth atlas, with 3 dispatches |
| 4 | Decal buffers (DBuffer) | 3 targets (2 BGRA8, 1 RG8), reads depth |
| 5 | G-buffer base pass | 317 draws, 39 pixel shaders, 6 targets: `RGBA16F` scene color, `RGB10A2` normals, and 4 BGRA8 targets; reads the decal buffers |
| 6 | Custom depth | 47 draws into a second 640x360 depth target |
| 7–11 | Ambient occlusion | 320x180 steps, an `R8` result at 640x360 |
| 12–19 | Shadow projection and lighting | Stencil clear, light attenuation (BGRA8, reads the shadow atlas), lighting into scene color, 1 dispatch that reads the G-buffer |
| 20–23 | Screen-space reflections | 320x180 `RGBA16F` steps, composite at 640x360 |
| 24–28 | Translucency and distortion | 19 + 6 + 2 draws, then composites |
| 29–35 | Depth of field or motion blur | 320x180 ping-pong between 3 targets, then a 640x360 composite |
| 36–55 | Bloom and eye adaptation | `R11G11B10F` chain down from 320x180 to 5x6 and back up; a 1x1 `RG32F` exposure target |
| 57 | Tonemapping | Into `T1`, 640x360 `RGB10A2`, reading the scene and bloom |
| 58–93 | UI preparation | 36 small passes (for example 133x12 and 83x10), each a clear plus 1 or 2 draws, probably text or widget caches |
| 94 | UI composite | 88 draws, 13 pixel shaders into `T1` |

For the renderer specification, this frame log is the pass-order source. The `.gputrace` can confirm each pass visually in Xcode.

### Second frame capture: native resolution and a 99% scale (2026-10-17)

The user set the output to the display's 3456x2234 and `sg.ResolutionQuality=99`, then captured frame 4276 (anti-aliasing quality 0).

- The scene targets are 3424x2216, and the output target is 3456x2234. So the 99% scale took effect.
- The 200% scale in the first capture had no effect, which fits UE4 4.17 limiting the scale to at most 100%. This is unverified.
- The frame has 95 passes instead of 94. Tonemapping now writes a scene-sized target (3424x2216). Then one extra pass, 3 draws with 3 pixel shaders, writes the 3456x2234 output from it. That pass is the scaling step. In the 100% frame, tonemapping wrote the output directly.
- The order and kinds of all other passes match the 640x360 frame.

This session also raised the pixel shaders seen in real pairs to 1,246 of 1,936.

## Xcode replay of frame C (2026-10-17)

Xcode 27.2 replayed frame C's `.gputrace` (Summary: 3 command buffers, 107 render encoders, 5 compute and 2 blit encoders, 1,027 draws, 6 dispatches). Three findings:

1. **No per-pass GPU timing.** Xcode reports "Profiling not available for Mesh pipelines". DXMT runs the frame's 8 geometry-shader draws as mesh pipelines, so the whole capture cannot be profiled. A frame without geometry-shader draws, or a capture with those draws removed, is needed for timings.
2. **The output is upscaled to 6912x4468 with MetalFX.** The capture contains `MTLFXSpatialScaler` work, and the final drawable is 6912x4468 in `RGB10A2Unorm`. The display has 3456x2234 pixels, so this image has 4 times the display's pixels and must be scaled down again by the system. Cause (found later): the user's `~/.zshrc` exported `DXMT_METALFX_SPATIAL_SWAPCHAIN=1`, and every launch from a shell inherited it. DXMT then doubles the output size (its default factor is 2). The SC6-Rosetta bottle loads CrossOver Preview's stock DXMT, not the `dxmt-SC6` build, so the SC6 profile in `dxmt-SC6` is not the cause. An earlier guess that the bottle's Retina mode doubles it was wrong too. The user's later setting of 1728x1117 is upscaled to exactly the display's 3456x2234.
3. **Bandwidth insights (550.64 MiB estimated savings in total):**
   - Many full-size targets cannot use Apple's lossless compression, because their usage includes `ShaderWrite`. Examples: one 3424x2216 `RGBA16Float` target (59.06 MiB) and several 29.53 MiB targets. DXMT sets `ShaderWrite` only when the game creates a texture with `D3D11_BIND_UNORDERED_ACCESS` (`d3d11_resource_helper.cpp`), so the game asks for it.
   - Some consecutive render encoders could be coalesced, for example encoder 69 (58.90 MiB) and encoder 16 (57.89 MiB).
   - API usage: 2 empty blit encoders and 1 late buffer creation. Minor.

## GPU profile of a frame without geometry-shader draws (2026-10-17, 10:05)

Capture: `SoulcaliburVI_F.2721_10'05'37_10-17-26.gputrace` with frame log `frame-20261017-100537-0.tsv`. The game ran at 1728x1117 (scene targets 1712x1108), Training stage, through `shader-capture.sh launch-profile`. That mode sets `SC6VRR_SKIP_GS_DRAWS=1`, so the DLL drops every draw that has a geometry shader bound. The frame log records 8 skipped draws: passes 13, 16, and 18 (the 64x64 volume targets) and pass 56 (the 32x32 target). Xcode then profiled the replay ("Profile after replay").

Limits of this profile:

- The replayed image is black except for the UI. Inferred cause: pass 56 builds the color-grading table (a 32x32x32 volume), its draw was skipped, and tonemapping then reads an empty table. Passes before tonemapping are not affected. The tonemapping cost should be about the same, because the shader work does not change.
- Xcode ran with profiler mode "Full", performance state "Medium", and overlapping execution. So the GPU was not at its highest clock, and the times below can differ from times during play.
- Percentages are Xcode's encoder costs. The milliseconds are those percentages of the 16.53 ms effective GPU time. Pass numbers come from `tests/frame_passes.py`, matched to encoders by order, thumbnails, and targets.

| Pass (frame log) | Role (inferred) | Cost | About |
| --- | --- | --- | --- |
| 5 | G-buffer (323 draws, 6 targets) | 33.31% | 5.5 ms |
| 1 | Depth prepass (275 draws) | 5.77% + 0.56%, plus small encoders | 1.05 ms or more |
| 19 | One compute dispatch, 214x121 groups, reads the G-buffer | 6.35% | 1.05 ms |
| 10 | Full-size ambient occlusion | 5.01% | 0.83 ms |
| 3 | Shadow depths, 4096x2048 | 4.88% | 0.81 ms |
| 25 | 6 draws into a new full-size target (separate translucency) | 4.85% | 0.80 ms |
| 17 | Lighting into scene color | 4.68% | 0.77 ms |
| 24 | 20 draws into scene color (translucency) | 4.63% | 0.77 ms |
| 14 | Full-screen lighting (2 draws) | 3.05% | 0.50 ms |
| 54, 55 | Largest two bloom upsample steps | 1.24% + 1.25% | 0.41 ms |
| 9, 15, 6, 29 | Half-size AO, shadow projection, custom depth, one full-size pass | 1.83%, 1.61%, 1.48%, 1.33% | 1.03 ms together |
| 95, 57 | UI composite into the back buffer, tonemapping | 1.27%, 1.20% | 0.41 ms |
| (DXMT) | Copy to the drawable after MetalFX | 1.33% | 0.22 ms |

**MetalFX is the largest single cost after the G-buffer.** Xcode lists the MetalFX spatial scaling step outside the encoder list, with a total duration of 3.98 ms. Its output is 3456x2234 `RGB10A2Unorm`. Compare that with the 16.53 ms of all game encoders: the upscale adds about 24% on top. This is unverified as pure GPU time, because "total duration" can include time that overlaps other work.

**The mid-frame `waitUntilCompleted` does not stop the game.** Command buffer 2 holds the depth prepass, and the capture shows `commit` and then `waitUntilCompleted` on it. An earlier reading of this as a CPU stall was wrong. In DXMT (`src/dxmt/dxmt_command_queue.cpp`), `waitUntilCompleted` runs on a separate thread (`dxmt-finish-thread`) that only recycles finished command buffers. The split into two command buffers comes from a flush: the last draws before it are small boxes with no pixel shader, which fits UE4's occlusion queries. UE4 is inferred to ask for a flush after it issues them.

The game's render thread can still wait in two places in DXMT:

1. `Map` of a staging resource blocks until the GPU is done with it (`d3d11_context_imm.cpp`, counted as "Sync" on the DXMT HUD).
2. `GetData` on a query does not block, but it returns "not ready" until the GPU has finished. UE4 is inferred to poll last frame's occlusion results in a loop until they are ready.

The DLL now measures both (see below).

Possible next steps:

1. Done 2026-10-17: MetalFX is off. The `~/.zshrc` line is commented out, and `tests/run.sh` and `tests/shader-capture.sh` pass `DXMT_METALFX_SPATIAL_SWAPCHAIN=0`. DXMT enables MetalFX only when the value is `1`.
2. Measure the render thread's waits in `GetData` and `Map` during play (below).
3. G-buffer cost is a third of the frame. The two most expensive pixel shaders in the frame cost 8.21% and 7.60%. They are probably material shaders of that pass, but this is unverified. They are candidates for checking in the converted Metal code.

## What done means for the first milestone

The native host draws one replayed battle frame that matches the DXMT capture within a stated image tolerance on an M1 Pro.
That establishes shader conversion and pass reproduction for one frame. It does not establish a full renderer, asset loading, or frame rate.

## Render-thread wait counters (2026-10-17)

In shader-capture mode, the DLL hooks `ID3D11DeviceContext::GetData`, `Map`, and `Flush`. Every 120 frames it writes one `waits:` line to the sc6vrr log with per-frame averages:

- **Query waits:** the time from a query's first "not ready" result to its ready result on the same query. Present ends any open wait, so one wait never spans two frames.
- **`GetData` calls** and how many returned "not ready".
- **`Map` time:** the time spent inside each `Map` call, and the longest one.
- **`Flush` calls.**

The F10 frame log also gets `QueryWait`, `Map`, and `Flush` lines. Each wait costs the encoding thread time that it cannot spend on the next frame. If query waits are large, a DXMT change can return occlusion results sooner, or report them as visible without waiting.

### First run (2026-10-17 10:27, menus and one battle, MetalFX off)

Log: `sc6vrr-20261017-102705.log`, 74 `waits:` lines of 120 frames each. During the battle (lines 19 to 73):

| Measure | Typical per frame | Highest 120-frame average |
| --- | --- | --- |
| Query waits | 0.5 to 2 ms, about 1 wait | 12.3 ms (43,053 `GetData` calls) |
| Longest single query wait | 3 to 8 ms | 117.9 ms (a load or scene change, inferred) |
| `Map` | 0.5 to 0.6 ms in about 1,100 to 1,300 calls; longest usually under 0.3 ms | 3.7 ms, with one 167 ms call |
| `Flush` | about the same count as "not ready" results | |

What this shows:

1. About once per frame, the render thread polls one query until it is ready. Each millisecond of waiting is about 3,500 `GetData` calls, so the game spins on the CPU rather than sleeping.
2. The `Flush` count matches the "not ready" count. Inferred: DXMT's `GetData` calls `Flush` through the vtable on every "not ready" result (`d3d11_context_imm.cpp`), so the hook counts DXMT's flushes, not the game's. After the first one, these flushes have no new commands to send.
3. The query is an event query (confirmed in the second run below). Inferred: it is UE4's per-frame GPU sync, which stops the CPU from getting too far ahead of the GPU. A long wait therefore means the GPU is behind. It is not wasted DXMT work, and making `GetData` return sooner would not raise the frame rate.
4. `Map` costs about 0.5 ms of CPU per frame. Almost none of that is waiting for the GPU.

Conclusion: the render thread's waits measure GPU time, so the GPU work in the profile above is the target, led by the G-buffer pass. A DXMT change to poll more cheaply would save CPU power, but not frame time.

### Second run: query kinds (2026-10-17 10:32, menus and one battle)

Log: `sc6vrr-20261017-103259.log`, 69 `waits:` lines. Each wait now records the query kind from `GetDesc`.

| Kind | Average wait per frame | Share of query wait time |
| --- | --- | --- |
| Event | 0.700 ms | 98.3% |
| Occlusion | 0.011 ms | 1.5% |
| Timestamp disjoint | 0.001 ms | 0.2% |

This confirms point 3 of the first run. The render thread waits on an event query, which is UE4's way to wait until the GPU finishes earlier work. Occlusion results are almost always ready when the game asks. So no DXMT change to queries would raise the frame rate. The GPU work is the target.
