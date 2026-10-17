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

Frame-rate changes are off. The engine hooks install only with `SC6VRR_FRAME_MODS=1`, so test cases 10 to 19 need that variable before they work again.

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
Fast mode sets `SC6VRR_FRAME_MODS=1` and `SC6VRR_UNCAP=1` without the 60 Hz gate, so the battle advanced one step per rendered frame, at about 190 to 240 fps.

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

## What done means for the first milestone

The native host draws one replayed battle frame that matches the DXMT capture within a stated image tolerance on an M1 Pro.
That establishes shader conversion and pass reproduction for one frame. It does not establish a full renderer, asset loading, or frame rate.
