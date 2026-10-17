# R-001: Renderer frame structure

Status: draft 2, 2026-10-17. Rules check recorded in [REVIEW.md](REVIEW.md). Not yet released.

This specification describes the order and shape of the render passes in one SoulCalibur VI battle frame: what each pass draws into, what it reads, and how large its targets are. It does not yet describe what each pass computes inside its shaders, or the values the game feeds them.

## Evidence

| Capture | Output size | Resolution scale | Notes |
| --- | --- | --- | --- |
| Frame A (2026-10-17 09:25, Training) | 640x360 | 100% | 94 passes with work, 982 draws, 4 compute dispatches |
| Frame B (2026-10-17 09:36, Training) | 3456x2234 | 99% | 95 passes with work, 1,016 draws, 4 compute dispatches |
| Frame C (2026-10-17 09:49, Training) | 3456x2234 | 99% | Same 95 passes as frame B, 1,019 draws. Also records depth-stencil, rasterizer, and sampler states, and vertex shader texture bindings |

Both captures are one-frame logs from the analysis DLL, with a matching Metal capture each. They stay in the private investigation folder.
Statements marked **observed** hold in both frames unless the text names one frame. Statements marked **inferred** interpret the observations and need confirmation.

## Terms

- **Output size (O):** the size of the swap chain image that the game presents.
- **Resolution scale (s):** the game's resolution setting, as a fraction. Observed values above 100% had no effect, so s is at most 1 (inferred: the engine limits it).
- **Scene buffer size (B):** the size of the 3D render targets. Observed: B is O × s, with each side rounded up to a multiple of 8. Frame A: 640x360. Frame B: 3424x2216.
- **View rectangle (V):** the part of the scene buffer that the 3D view covers. Observed: in frame B, V is 3424x1929 at offset (0, 143), a letterbox inside B. In frame A, V fills B.
- **Full-screen pass:** a pass whose draws cover the target with 3 to 13 vertices, a triangle or rectangle, with no scene geometry.
- **Depth convention:** observed in frame C: the scene and custom depth targets are cleared to 0, and their depth tests pass on greater-or-equal. So the scene uses reversed depth, where 1 is near and 0 is far. The shadow atlas is the exception: it is cleared to 1 and tested less-or-equal.

## Targets

Formats use their Direct3D names, because the original game's targets are defined that way. A `TYPELESS` format is viewed with a concrete format when it is bound.

| Role (inferred) | Size | Format | Written or bound in stages | Read in stages |
| --- | --- | --- | --- | --- |
| Scene depth and stencil | B | 24-bit depth, 8-bit stencil (`R24G8_TYPELESS`) | 1; bound as depth in 4, 5, 8, 11 | 2, 4, 7–12, 14 |
| Hierarchical depth | Next power of two of V, halved, per side (observed: 512x256 and 2048x1024), with mips until the shorter side is 1 (9 and 11 levels observed) | `R16_FLOAT` | 2 | 2, 7, 11 |
| Shadow depth atlas | 4096x2048, fixed | 16-bit depth (`R16_TYPELESS`) | 3 | 8 |
| Decal buffers, 3 targets | B | 2 × `B8G8R8A8`, 1 × `R8G8_UNORM` | 4 | 5 |
| Scene color | B | `R16G16B16A16_FLOAT` | 5, 7, 8 | 9 |
| G-buffer normal | B | `R10G10B10A2_UNORM` | 5 | 7–9 |
| G-buffer 2 to 5, 4 targets | B | `B8G8R8A8` | 5 | 7–10 (the last one only in 8) |
| Second depth (custom depth) | B | 24-bit depth, 8-bit stencil | 6 | 12 |
| Stencil copy | B | `R16_UINT` | 7 | 8 |
| Ambient occlusion steps, 2 targets | B/2 | `R16G16B16A16_FLOAT`, `B8G8R8A8` | 7 | 7 |
| Ambient occlusion result | B | `R8_UNORM` | 7 | 7–9 |
| Light attenuation | B | `B8G8R8A8` | 8 | 8 |
| Reflection steps, 3 targets | B/2 wide, V/2 tall | `R16G16B16A16_FLOAT` | 10 | 10 |
| Lit scene | B | `R16G16B16A16_FLOAT` | 10, 11 | 11, 12 |
| Separate translucency | B | `R16G16B16A16_FLOAT` | 11 | 14 |
| Distortion buffer and its composite | B | `B8G8R8A8`, `R16G16B16A16_FLOAT` | 11 | 11 |
| Pre-post-process composite | B | `R16G16B16A16_FLOAT` | 12 | 13 |
| Post-process steps, 3 targets | B/2 | `R16G16B16A16_FLOAT` | 13 | 13, 14 |
| Post-process composite | B | `R16G16B16A16_FLOAT` | 14 | 15, 16 |
| Bloom chain | B/2, B/4, … down to about B/128 | `R11G11B10_FLOAT` | 15 | 15, 16 |
| Exposure value | 1x1 | `R32G32_FLOAT` | 15 | 15, by the vertex shader of the bloom setup pass (observed in frame C) |
| Tonemapped scene | B, only when s < 1 | `B8G8R8A8` | 16 | 17 |
| Output image | O | `R10G10B10A2_UNORM` | 16 or 17, then 19 | Presented |
| UI caches, about 36 small targets | Text-sized, for example 720x65 at O = 3456x2234 | `B8G8R8A8` | 18 | 19 |

The stage numbers refer to frame B. Frame C has the same stages. Frame C also records vertex shader texture bindings; they add only the exposure read above.

Sizes written as B/2 are half of B in each side, rounded up. All targets have 1 sample. No pass uses multisampling.

## Stages

Each stage is one or more consecutive passes. A pass starts whenever the bound color or depth targets change. The pass counts below are from frame B.

| Stage | Passes | Role (inferred) | Work (observed) |
| --- | --- | --- | --- |
| 1 | 1 | Depth prepass | About 276 draws of scene geometry into scene depth only, after a depth clear to 0. About 1.3 million indices. 8 pixel shaders. |
| 2 | 2 | Hierarchical depth build | One full-screen draw per mip level (9 in frame A, 11 in frame B), reading scene depth and the hierarchical depth target itself. |
| 3 | 3 | Shadow depths | About 88–95 draws into the shadow atlas, after a clear. 3 compute dispatches run inside this pass. |
| 4 | 4 | Decals | 12 draws into the 3 decal buffers, after clearing them, with scene depth bound. |
| 5 | 5 | G-buffer base pass | About 320 draws into 6 targets at once (scene color, normal, 4 more), after clearing all 6. Scene depth is bound. Reads the decal buffers. 39 pixel shaders. |
| 6 | 6 | Custom depth | 47 draws into the second depth target, after a clear. |
| 7 | 7–11 | Ambient occlusion | 5 full-screen passes: a stencil copy, two half-size steps, the full-size result, and an apply step into scene color. |
| 8 | 12–18 | Shadowing and lighting | A stencil clear, three 64x64 passes without color targets (inferred: lookup tables), light attenuation (reads the shadow atlas), and lighting into scene color. |
| 9 | 19 | Tiled compute lighting or reflections | 1 compute dispatch over 8x8-pixel tiles of V (observed: 80x45 and 428x242 groups). Reads scene depth, scene color, and the G-buffer. |
| 10 | 20–23 | Screen-space reflections | 3 half-size passes, then a full-size composite into the lit scene. |
| 11 | 24–28 | Translucency and distortion | About 19–24 draws into the lit scene with depth bound, in frame B within the letterboxed viewport. Then separate translucency (6 draws), a distortion buffer (2 draws), and two composites. |
| 12 | 29 | Pre-post-process composite | 1 full-screen pass reading scene depth, custom depth, and the lit scene. |
| 13 | 30–34 | Depth of field or motion blur (inferred) | Half-size ping-pong between 3 targets. |
| 14 | 35 | Post-process composite | 1 full-screen pass reading scene depth, separate translucency, and the half-size results. |
| 15 | 36–55 | Bloom and exposure | A 1x1 exposure pass. A chain that halves the size 7 times, then combines back up level by level. |
| 16 | 56–57 | Tonemapping | A 32x32 pass without a color target (inferred: a lookup table). Tonemapping reads the scene and bloom. When s = 1, it writes the output image directly. |
| 17 | 58 (frame B only) | Scaling | When s < 1, one pass writes the output image from the tonemapped scene: 3 draws, 3 pixel shaders, viewport O. |
| 18 | 59–94 | UI preparation | About 36 small passes. Each clears a small target and draws 1–2 quads, in pairs where the second reads the first. |
| 19 | 95 | UI composite | About 88 draws into the output image, reading the UI caches. 13 pixel shaders. |

## Pipeline facts

- **Topology (observed):** 976 of 1,016 draws in frame B use triangle lists, and 40 use triangle strips. No draw uses points, lines, or patches.
- **Blend states (observed):** 8 blend states appear. Two of them cover 820 of the draws, which suggests opaque writes in the depth prepass and G-buffer. This is inferred.
- **Compute (observed):** 4 dispatches per frame. One grid scales with V in 8x8-pixel tiles. Two grids are 3x2x8 at V = 640x360 and 14x8x8 at V = 3424x1929, so their x and y counts grow with V. One dispatch grows from 60 to 1,674 groups between the frames.
- **Viewports (observed):** almost every pass uses a viewport equal to its target size. The exception is the first translucency pass in stage 11, which uses V in frame B. The reflection step targets are themselves B/2 wide and V/2 tall.
- **Clear values (observed in frame C):**

| Target | Clear |
| --- | --- |
| Scene depth and stencil, stage 1 | Depth 0 and stencil 0 |
| Scene stencil, stage 8 | Stencil 0 only |
| Shadow depth atlas | Depth 1 |
| Custom depth | Depth 0 |
| Decal buffers 1, 2, 3 | (0, 0, 0, 1), (0.5, 0.5, 0.5, 1), (0, 1, 0, 1) |
| Scene color | (0, 0, 0, 1) |
| G-buffer normal and G-buffer 2 to 5 | (0, 0, 0, 0) |
| Light attenuation | (1, 1, 1, 1) |
| Separate translucency | (0, 0, 0, 1) |
| Distortion buffer, UI caches | (0, 0, 0, 0) |

## Depth and stencil states

Observed in frame C. Enum names are the Direct3D 11 ones. "Stencil ops" lists the fail, depth-fail, and pass operations, then the comparison.

| Used in (stage) | Depth test | Depth write | Stencil | Purpose (inferred) |
| --- | --- | --- | --- | --- |
| 1, 6 | Greater-or-equal | On | Off | Depth prepass and custom depth |
| 1, 4, 11 | Greater-or-equal | Off | Off | Depth-tested drawing without depth writes |
| 3 | Less-or-equal | On | Off | Shadow depths |
| 5 | Equal, or greater-or-equal | Off | Write mask 240 (upper 4 bits), replace on pass, compare always, reference 128 or 160 | G-buffer: draws only where the prepass depth matches, and marks per-object bits in the stencil |
| 4, 11 | Off | Off | Replace on every outcome, compare always | Stencil marking for decals and translucency |
| 8 | Off | Off | Zero on every outcome, compare not-equal | Stencil reset inside light passes |
| 8 | Greater-or-equal | Off | Front faces increment on depth fail, back faces decrement on depth fail | Light volume test (depth-fail stencil) |
| 11 | Greater-or-equal, then off | Off | Masks 1, reference 1: replace on pass, then compare equal, then zero on pass | Distortion: mark, use, and clear stencil bit 0 |
| All full-screen passes | Off | Off | Off | Post-processing |

## Rasterizer states

Observed in frame C. All three are solid fill, counter-clockwise front faces, depth clipping on, scissor test on, no depth bias, and the multisample flag set.

| Culling | Used in (stage) |
| --- | --- |
| None | Almost every pass, including all full-screen passes |
| Back faces | Scene geometry in stages 1, 3, 5, 6, 11 |
| Front faces | Stages 4 and 8 (inferred: decal and light volumes drawn from inside) |

The shadow pass uses no rasterizer depth bias, so any bias must come from its shaders. This is inferred.

## Samplers

Frame C created 38 unique sampler states. Four of them cover most bindings:

| Filter | Addressing | Bindings in the frame (approximate) |
| --- | --- | --- |
| Anisotropic, maximum 8 | Wrap | 80 (inferred: material textures) |
| Bilinear (linear minify and magnify, point mip) | Clamp | 55 |
| Point | Clamp | 45 |
| Trilinear | Clamp | 9 |

None of the four uses comparison filtering. The per-pass sampler slots are in the frame log and still need to be written into the stage descriptions.

## Variations between the two frames

- With s < 1, tonemapping writes a scene-sized target, and stage 17 scales it to O. With s = 1, stage 17 is absent.
- In frame B, most full-screen passes use 2 draws instead of 1 (13 or 16 vertices instead of 3). Inferred: the extra draw covers the area outside V.
- Draw counts in stages 1, 3, and 5 differ slightly, because the scene content differs between the frames.

## Not covered yet

The implementation cannot build a frame from this specification alone. These parts are still missing:

1. Constant data for every pass, for example camera matrices, light parameters, and exposure. The frame log does not record buffer contents.
2. Which texture slot and sampler slot each pass uses, per stage. Frame C records them, but this specification does not list them yet.
3. Which game shader each pass uses. The captures identify them, but the specification must still describe how the new program finds them in the user's installation.
4. The UI passes in detail, and confirmation of every inferred role, for example in the Metal capture.
