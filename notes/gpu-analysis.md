# GPU work in SoulCalibur VI, and how to reduce it

Summary: SC6 draws with stock Unreal Engine 4.17 render passes. Its own look comes from materials, particle effects, and per-stage settings, not from custom engine code. The user's settings are already at "medium" for shadows, post-processing, and effects. The biggest remaining GPU levers are the 3D render resolution, the resolution of transparent effects, and a bandwidth cost inside DXMT. Geometry-shader and tessellation emulation in DXMT should not run in normal battles.

Scope: static analysis only, done on 2026-10-15. I did not launch the game. Every saving below is an **estimate** from reasoning, not a measurement. The section "How to measure" lists the test that confirms each one.

Background assumption: all savings matter for frame rate only if the battle is GPU-bound. That is not measured yet (the main menu shows 8.41 ms of GPU time per frame; the battle runs at 65–86 fps). If the battle is CPU-bound, these changes still lower GPU load, power, and heat, but they do not raise the frame rate. Test 0 below decides this.

## 1. What I looked at

| Source | What it gave |
| --- | --- |
| `pakchunk0` configs: `Engine/Config/Base*.ini`, `SoulcaliburVI/Config/Default*.ini`, `Windows/*.ini` | Project renderer settings, device profiles, scalability levels |
| The bottle's `Saved/Config/WindowsNoEditor/*.ini` | The user's chosen quality levels |
| `Engine/GlobalShaderCache-PCD3D_SM5.bin` | All engine-owned shaders (1,112 programs) |
| 19,756 material packages from both paks | All material shader maps (the compiled shaders of each material) |
| 2,915 particle-system packages | Which effects simulate particles on the GPU |
| 99 stage, menu, and cutscene maps | Lights, their mobility and shadow flags, and post-process volume settings |
| Ghidra (`re/probe.sh`) | Default values of key console variables, and how the menu stores settings |
| DXMT source, branch `sc6/native-fullscreen` | Which D3D11 features DXMT emulates, and how it starts and ends render passes |

The analysis scripts and the small material extractor (a Rust program on the repak library) are in the shared notes folder under `gpu-analysis-tools/`. They take the AES key as an argument, so no key is stored in them.

## 2. Current renderer settings

### 2.1 How the settings are chosen

UE4 groups many console variables (cvars, named engine settings such as `r.ShadowQuality`) into **scalability groups** (`sg.*`, one level from 0 to 3 per group). The in-game graphics menu writes these levels to `GameUserSettings.ini`, and each level expands to the cvars in `BaseScalability.ini`. SC6's own `WindowsScalability.ini` changes only the view-distance and foliage groups, and it sets them to the same values at every level.

The user's file has these levels:

| Group | Level | Meaning |
| --- | --- | --- |
| `sg.ResolutionQuality` | 100 | 3D rendering at 100 % of the window |
| `sg.ViewDistanceQuality` | 3 | No effect (SC6 sets equal values at all levels) |
| `sg.AntiAliasingQuality` | 0 | `r.PostProcessAAQuality=0`, so FXAA does not run |
| `sg.ShadowQuality` | 1 | Medium shadows |
| `sg.PostProcessQuality` | 1 | Medium post-processing |
| `sg.TextureQuality` | 3 | Highest textures, 8x anisotropic filtering |
| `sg.EffectsQuality` | 1 | Medium effects |
| `sg.FoliageQuality` | 3 | No effect (SC6 sets equal values at all levels) |

The bottle's `Engine.ini` has no `[SystemSettings]` section. So the repo file `config/Engine.SystemSettings.ini` is **not installed** yet, and none of its overrides are active.

The project files add four fixed settings (`DefaultEngine.ini`): FXAA as the default anti-aliasing method, the legacy tonemapper (`r.TonemapperFilm=0`), `r.EarlyZPassOnlyMaterialMasking=True`, and `r.TextureStreaming=0`. The Windows device profile sets only `r.TextureStreaming=0`. The PS4 profile renders at 83 % resolution, which shows that the developers accepted a lower render resolution on console.

### 2.2 Per-stage post-process volumes

A **post-process volume** is a map object that sets image effects (bloom, ambient occlusion, depth of field, and others) for the camera inside it. I read all 45 volumes in the stage lighting maps:

| Setting | Value in the volumes | Consequence |
| --- | --- | --- |
| Motion blur amount | 0 in all 45 | The motion-blur pass and the velocity pass do not run. |
| Lens flare intensity | 0 in all 45 | No lens-flare pass |
| Screen-space reflection intensity | 0 in 42; the others keep the default | SSR is off anyway, because effects level 1 sets `r.SSR.Quality=0`. |
| Auto exposure | Min = Max = 1.0 in 44 | Exposure is fixed. Also `r.EyeAdaptationQuality=0` at post-process level 1. |
| Ambient occlusion | Intensity 0.7–0.9, radius 40–90 | SSAO runs in every stage. |
| Bloom | Intensity 0.6–1.5, **size scale 40** (engine default 4) | Bloom runs with very wide blur kernels. |
| Depth of field | Gaussian method in 4 volumes (`STG004_C`, `STG013` day, evening, night) | DoF runs only in those stage variants, and in cinematics that call `LuxEnableCameraBokeh`. |

### 2.3 Every cvar that changes GPU cost

"Current" is the value that applies today, from the levels in 2.1 plus the volumes in 2.2.

| Area | Cvar | Current | What it costs, and what a change would do |
| --- | --- | --- | --- |
| Resolution | `r.ScreenPercentage` | 100 | Scales every per-pixel pass. The strongest lever (candidate 1). |
| Upscale filter | `r.Upscale.Quality` | 2 | Used only when the screen percentage is below 100. |
| Anti-aliasing | `r.PostProcessAAQuality` | 0 (off) | FXAA is off. Turning it on would cost time (about 0.2 ms, estimate). |
| Shadows | `r.ShadowQuality` / `r.Shadow.CSM.MaxCascades` / `r.Shadow.MaxCSMResolution` | 3 / 1 / 1024 | Already low: one 1024² cascade for one directional light. Little left to save. |
| Shadows | `r.Shadow.DistanceScale`, `r.LightMaxDrawDistanceScale` | 0.7, 0.5 | Already reduced |
| Distance-field shadows, AO, GI | `r.DistanceFieldShadowing`, `r.DistanceFieldAO` | 0, 0 | Off |
| Volumetric fog | `r.VolumetricFog` | 0 | Off |
| Ambient occlusion | `r.AmbientOcclusionLevels` / `r.AmbientOcclusionMaxQuality` / `r.AmbientOcclusionMipLevelFactor` | −1 (automatic) / 60 / 1.0 | SSAO runs every frame (candidate 4). |
| Screen-space reflections | `r.SSR.Quality` | 0 | Off |
| Bloom | `r.BloomQuality` / `r.FastBlurThreshold` / `r.Filter.SizeScale` | 4 / 2 / 0.7 | Runs every frame with wide kernels (candidate 5). |
| Depth of field | `r.DepthOfFieldQuality` | 1 | Runs only where a volume or a cinematic turns it on. |
| Motion blur | `r.MotionBlurQuality` | 3 | Inactive, because every volume sets the amount to 0. |
| Lens flare, fringe, light shafts | `r.LensFlareQuality`, `r.SceneColorFringeQuality`, `r.LightShaftQuality` | 0, 0, 0 | Off |
| Tonemapper | `r.Tonemapper.Quality` | 2 | Level 2 adds the vignette. Level 1 would save very little (the tonemapper pass runs anyway). |
| Subsurface scattering | `r.SSS.Scale` / `r.SSS.Quality` / `r.SSS.HalfRes` / `r.SSS.SampleSet` | 0.75 / 0 / 1 / 0 | Runs on character skin (84 character materials use a subsurface profile). Already the cheap settings. |
| Translucency lighting | `r.TranslucencyLightingVolumeDim` / `r.TranslucencyVolumeBlur` | 32 / 0 | Two 32³ volumes. Small cost. |
| Refraction, distortion | `r.RefractionQuality` | 0 | Off, so the 17 refraction and 10 distortion materials do not distort. |
| Separate translucency | `r.SeparateTranslucency` / `r.SeparateTranslucencyScreenPercentage` | 1 / 100 | Most effects draw at full resolution (candidate 2). |
| Particles | `r.EmitterSpawnRateScale` / `r.ParticleLightQuality` | 0.25 / 0 | Spawn rate is already cut to a quarter. Particle lights are off. |
| Material quality | `r.MaterialQualityLevel` / `r.DetailMode` | 2 (medium) / 1 | Already medium |
| Scene color | `r.SceneColorFormat` | 3 (R11G11B10) | Already the compact format |
| Textures | `r.MaxAnisotropy` | 8 | Going to 4 saves a little texture filtering (about 0.1 ms, estimate). |
| Depth prepass | `r.EarlyZPass` / `r.EarlyZPassMovable` | 3 (engine heuristic) / on | Do not change. See section 6. |
| Occlusion | `r.HZBOcclusion` / `r.AllowOcclusionQueries` | 0 / 1 | Stock hardware occlusion queries. No GPU gain expected. |
| Custom depth | `r.CustomDepth` | 1 (on demand) | Stock default |

## 3. Shaders in the paks

UE 4.17 stores shaders inside the packages, compressed with zlib. There is no separate shader archive (no `ShaderArchive-*` files). I inflated every stream and read the DXBC header (the D3D11 shader bytecode format) of each program.

| Source | Pixel | Vertex | Compute | Geometry | Hull | Domain |
| --- | --- | --- | --- | --- | --- | --- |
| Global shader cache (unique) | 553 | 82 | 339 | 4 | 0 | 0 |
| Material shader maps (unique, 4,318 materials) | 21,038 | 1,924 | 458 | 30 | 21 | 39 |

All programs are shader model 5.0.

### 3.1 Geometry shaders and how DXMT runs them

DXMT has two paths for a geometry shader (GS, a stage that can create or route triangles after the vertex shader):

1. **Pass-through GS.** If the GS only copies its inputs and sets the render-target layer, DXMT folds it into the vertex stage. This costs almost nothing. The rules are in `CheckGSBBIsPassThrough` and `CheckGSSignatureIsPassThrough` (`src/airconv/dxbc_converter.cpp`): only `mov`/`emit`/`cut` instructions, no bound resources, and no more outputs than inputs.
2. **Emulated GS.** Every other GS becomes a Metal mesh-shader pipeline. The vertex shader runs as the object stage and the GS as the mesh stage (`src/d3d11/d3d11_pipeline_gs.cpp`). This adds GPU work and a separate pipeline for each combination.

I applied the same rules to every GS:

| Geometry shader | Where | DXMT path | When UE4 uses it | Runs in SC6? |
| --- | --- | --- | --- | --- |
| `FWriteToSliceGS` | Global | Pass-through (cheap) | Writes to volume textures: the color lookup table and the translucency lighting volume | Yes, every frame. The cost is small. |
| `FAtmosphereGS` | Global | Emulated (one more output than inputs) | Precomputes atmospheric fog tables | Once, when a map with atmospheric fog loads (`STG014_Lighting1`) |
| `FCopyShadowMapsCubeGS` | Global | Emulated | Copies cached cube shadow maps of point lights | No (needs a shadowed point light, see below) |
| `FLpvVisualiseGS` | Global | Emulated | Debug view of light propagation volumes | No |
| `FOnePassPointShadowDepthGS` (16 variants) | 771 materials | Emulated (loops, 18 output vertices) | Shadow maps of point lights that are stationary or movable and cast shadows | **No light like this exists** in any stage, menu, creation, or cutscene map. |
| `FConvertToUniformMeshGS` (14 variants) | All 4,318 materials with shaders | Emulated | Distance-field global illumination only | No, that feature is off |

The 458 material compute shaders are all `FEvaluateSurfelMaterialCS`. They also belong to distance-field global illumination, so they never run.

Lights in the maps: every stage has one stationary directional light with shadows, plus 2 or 3 stationary directional lights without shadows. All point and spot lights are static (baked into lightmaps) or have shadows off. A few spot lights are movable, but none of them casts shadows. Unverified: lights spawned at runtime by blueprints (for example, a light attached to a character or an effect). A Metal frame capture would show any mesh-shader draws.

Note for measuring: DXMT's HUD has a "G" flag for unsupported geometry draws, but this DXMT version never sets it (`UnsupportedGeometryDraw` is declared in `dxmt_statistics.hpp` and never raised). So the HUD cannot show whether the GS emulation runs. A frame capture can.

### 3.2 Tessellation

Hull and domain shaders exist in only three materials, all for the ocean of one DLC stage: `DLC/09/Stage/STG016/.../Ocean/Materials/M_OceanWater`, `MI_OceanWater`, and `MI_BASE_OceanWater`. They output triangles with fractional-odd partitioning. DXMT supports that case, through its own tessellation emulation (`src/d3d11/d3d11_pipeline_ts.cpp`). It flags only line or point outputs as unsupported. So tessellation costs extra GPU time only on STG016, and only for the ocean surface.

## 4. Game-specific GPU work

1. **No custom engine render passes.** The exe contains 177 shader source paths, and all of them are stock UE 4.17 files (plus CRI Mana video). The docs list only one `Lux.*` cvar, `Lux.Trace.ParallelUpdate`, which is not a render setting (`SC6-docs/docs/sc6/dev-debug-hooks.md`).
2. **Effects are materials and particles.** The VFX folders hold 241 translucent and 248 additive materials. Of 2,812 particle systems, 1,245 have at least one GPU emitter (an emitter whose particles the GPU simulates). These include the Soul Charge loops (`Eff_Sc6_Battle_Scrg_Loop_*`) and the Critical Edge effects (`Eff_Sc6_Chr*_Cri_*`). In addition, 1,767 systems use mesh particles, and 450 have particle lights (which `r.ParticleLightQuality=0` turns off). So the most expensive battle moments are probably the screen-filling effects, because every transparent layer is shaded and blended per pixel.
3. **Character shading.** Characters use subsurface-profile skin (84 character materials), anisotropic hair or metal (11 materials), and one cloth and one clear-coat material. The skin adds the screen-space subsurface pass.
4. **Cloth.** NvCloth and APEX cloth run on the CPU in UE 4.17 (APEX's GPU solver needs CUDA, which this system cannot run). So cloth is a CPU cost for the CPU-profiling session, not a GPU cost.
5. **Skinning.** The global cache has no GPU skin-cache shaders, so the skin cache is not compiled into this build. As a result, the vertex shader skins each character again in every pass that draws it: the depth prepass, the base pass, and the shadow pass.
6. **Cinematic hooks.** The sequencer events `LuxEnableCameraBokeh`, `LuxEnableRimLightParam`, and `LuxEnableColorCorrection` turn on depth of field, rim lighting, and color grading in cinematics. I did not trace what each sets.

## 5. Ranked candidates

Ways to apply a change:

- (a) a cvar in the bottle's `Engine.ini` `[SystemSettings]` section (merged from `config/Engine.SystemSettings.ini`). These values have higher priority than the scalability groups, so the menu cannot undo them. A restart is needed.
- (b) a `~mods` override pak, as in `mods/build-boot-skip.sh`
- (c) a hook in the `sc6vrr` DLL (`dll/src/sc6vrr.c`)
- (d) a change in the DXMT fork

A useful helper for (a) and the tests: the DLL could read a variable such as `SC6VRR_CVARS="r.ScreenPercentage=80;..."` and set those cvars at start through UE4's console manager. Ghidra shows the console-manager pointer at `0x14415cd80` (the cvar registration functions call through it). The slot of its "find variable" method is not located yet. With this helper, each test case could switch cvars without editing `Engine.ini`.

Frame budget for comparison: 8.33 ms at 120 fps, and 11.8 ms at 85 fps. The menu measurement is 8.41 ms of GPU time.

| Rank | Change | How | Expected GPU saving (estimate) | Visual cost |
| --- | --- | --- | --- | --- |
| 1 | Render the 3D scene at 80 % (`r.ScreenPercentage=80`) | (a) | 15–25 % of GPU time, so about 1.3–2.1 ms of the 8.4 ms | Softer 3D image. The UI stays sharp. |
| 2 | Draw separate translucency at half resolution (`r.SeparateTranslucencyScreenPercentage=50`) | (a) | 0.1–0.3 ms in calm frames, 1–3 ms during Soul Charge and Critical Edge | Softer effects, and possible halos where effects cross character edges |
| 3 | Skip useless tile loads in DXMT render passes | (c) + (d) | 0.3–0.8 ms | None, if done right |
| 4 | Lighter SSAO (`r.AmbientOcclusionLevels=1`) | (a) | 0.2–0.5 ms | Less broad shading in corners and under characters |
| 5 | Lighter bloom (`r.BloomQuality=3`) | (a) | 0.1–0.4 ms | Bloom changes shape. SC6's very wide bloom makes this visible. |
| 6 | Turn off depth of field (`r.DepthOfFieldQuality=0`, already in `config/Engine.SystemSettings.ini`) | (a) | 0.3–0.8 ms, but only in `STG004_C`, `STG013`, and cinematics | No blur in those scenes |
| 7 | Turn off screen-space subsurface scattering (`r.SSS.Scale=0`) | (a) | 0.2–0.4 ms when characters fill the screen | Skin looks harder |
| 8 | Lower anisotropic filtering (`r.MaxAnisotropy=4`) | (a) | About 0.1 ms | Floors look slightly blurrier at grazing angles |

### 5.1 Candidate 1: render resolution

1. The game renders at the window size, 1728 × 1084 (1.87 million pixels).
2. Most passes cost time in proportion to the number of pixels: the base pass, lighting, AO, bloom, and the transparent effects.
3. At 80 %, UE4 renders 1382 × 867 (1.20 million pixels, 64 % of the pixels). Its upscale pass then fills the 1728 × 1084 view, and MetalFX upscales that by 2.
4. I assume that 50–70 % of the GPU frame is per-pixel work. Then the saving is about (1 − 0.64) × 0.5–0.7, which is 18–25 %.
5. UE4 draws the UI after its upscale pass, at full resolution. So menus and the HUD keep their sharpness.

The cost is two upscale steps in a row (UE4's filter, then MetalFX). A cleaner long-term option is for DXMT to give the game a smaller back buffer and let MetalFX do all of the scaling. That needs DXMT to report a smaller client size to the game, which is more work.

This change is also the best way to answer the GPU-bound question (test 0).

### 5.2 Candidate 2: half-resolution effects

1. UE4 draws translucent materials in a separate translucency pass when the material allows it. The material default is on, and only 54 materials turn it off.
2. Additive and translucent effects are blended per pixel. A large Soul Charge or Critical Edge effect can cover the screen many layers deep.
3. At 50 %, the pass shades a quarter of the pixels. The result is upsampled with depth-aware filtering (`TranslucencyUpsampling.usf` is in the exe).
4. So the saving is small in calm frames and largest during big effects. Those frames are probably the ones that miss the frame budget.

Also try 70 % if 50 % looks too soft.

### 5.3 Candidate 3: tile loads in DXMT

Apple GPUs are tile-based: each render pass loads the old pixels of its targets from memory into on-chip tile memory, and stores them back at the end. The load is wasted when the pass overwrites every pixel anyway.

1. DXMT starts every render pass with `WMTLoadActionLoad` and ends it with `WMTStoreActionStore` (`src/d3d11/d3d11_context_impl.cpp`, the render-pass setup near line 4530).
2. It replaces the load with a clear only when a clear command comes first, and it merges some passes (`dxmt_context.cpp`, the `*_optimized` counters).
3. `DiscardView`, `DiscardView1`, and `DiscardResource` are not implemented. A comment there already says that they could become `LoadActionDontCare`.
4. UE4's post-process passes write new pooled targets with full-screen draws. Each of those loads data that it then overwrites. I expect 15–25 such passes per frame, most of them below full resolution.
5. At roughly 150 GB/s of usable bandwidth, 50–100 MB of wasted loads per frame cost about 0.3–0.7 ms.

How to apply it:

1. In DXMT (d): implement `DiscardView1` and `DiscardResource` so that the next render pass on that target uses `DontCare` for the load. This step is safe on its own, because nothing changes until a caller uses it.
2. In the DLL (c): UE4 knows when a target's old contents do not matter (render-target load action "no action"). The D3D11 back end of UE 4.17 ignores that hint. A hook in that function (the D3D11 render-target setup, statically linked in the exe; its address is not located yet) could call `DiscardView1` for those targets.

Measure first. Test 3 counts the passes and their load actions.

### 5.4 Candidates 4 and 5: SSAO and bloom

- SSAO: with `r.AmbientOcclusionLevels=-1`, UE4 picks the number of AO levels from the AO quality value in the volumes. Each level is a pass at a lower resolution. Level 1 keeps only one pass. The exact number of levels today is not measured.
- Bloom: SC6 uses a bloom size scale of 40, ten times the engine default. The blur sample count is capped (the global cache has filter variants up to 32 samples), so each bloom stage probably runs at the cap. I did not confirm which stage `r.BloomQuality=3` removes. If it removes the smallest stage, the saving is close to zero.

## 6. Changes I do not recommend

| Idea | Why not |
| --- | --- |
| Turn off the depth prepass (`r.EarlyZPass=0`) | SC6 sets `r.EarlyZPassOnlyMaterialMasking=True`. I think (from the UE 4.17 design, not verified in this exe) that masked materials then test their alpha mask only in the prepass, and the base pass relies on that depth. Without the prepass, masked surfaces (foliage, hair cards) would probably draw as solid. |
| Lower shadows further | Shadows are already one 1024² cascade for one light. |
| DXMT work on geometry-shader or tessellation speed | The emulated paths do not run in normal battles (section 3). Only the STG016 ocean uses tessellation. |
| Install `config/Engine.SystemSettings.ini` as a performance fix | The volumes and the medium levels already turn off most of what it disables. Its real effect is the depth-of-field part (candidate 6), and it protects against cinematics that turn effects on. |

## 7. How to measure

All tests use the same Training scene: one stage, the default camera, 30 seconds idle, then 10 seconds with Soul Charge active. Record the Metal HUD's GPU time per frame and the frame interval. `tests/cases/09-profile-battle.env` is a good base, because it already sets `DXMT_LOG_FRAME_STATS=1`.

| Test | Confirms | Method |
| --- | --- | --- |
| 0. GPU- or CPU-bound | Whether any GPU change can raise fps | Compare the HUD's GPU time with the frame interval. Then run once with `r.ScreenPercentage=50`. If fps rises clearly, the battle is GPU-bound. If fps stays the same, it is CPU-bound. |
| 1. Resolution | Candidate 1 | A/B with `r.ScreenPercentage=100` and `80`. Compare GPU ms idle and during Soul Charge. |
| 2. Effects resolution | Candidate 2 | A/B with `r.SeparateTranslucencyScreenPercentage=100` and `50`, mainly during Soul Charge and Critical Edge. In a frame capture, the separate translucency pass time should fall to about a quarter. |
| 3. Tile loads | Candidate 3 | Take a Metal frame capture: launch with `MTL_CAPTURE_ENABLED=1` and `DXMT_CAPTURE_EXECUTABLE=SoulcaliburVI.exe`, then press F10 in the fight (or set `DXMT_CAPTURE_FRAME=<n>`). The DXMT log prints where it saves the `.gputrace` ("A new capture will be saved to"). In Xcode, count the render passes that load a target and then fully overwrite it, and read the memory traffic per pass. `DXMT_LOG_FRAME_STATS=1` logs `render_passes=` and `clear_passes=` every 120 frames. |
| 4–7. Post effects | Candidates 4–7 | The same frame capture gives the GPU time of the AO, bloom, DoF, and subsurface passes. Then A/B each cvar. |
| GS check | Section 3.1 | In the frame capture, search for mesh-shader draws. None are expected in battle, except the atmosphere precompute at map load. |
| Tessellation check | Section 3.2 | Capture one frame on STG016 and read the time of the ocean draw. |

Each A/B result should go into `notes/test-results.md`, with the stage, the game state, and both GPU times.
