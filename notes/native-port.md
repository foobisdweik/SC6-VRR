# Native SoulCalibur VI investigation

Recorded on 2026-10-16. This is a design, not a completed port or a legal clearance.

The intended result is a native Apple silicon game that runs on the user's owned copy.
On 2026-10-16 the user chose a clean-room shape for the shipped code:

1. The analysis side studies the original executable and the running game.
2. The analysis side writes specifications that describe behavior in its own words.
3. The implementation side writes new code from those specifications only.
4. The shipped program contains only that new code and permissively licensed libraries.
5. At runtime, the shipped program loads game assets from the user's own installation.

This replaces the earlier recommendation of a recompilation-led hybrid.
A clean room (a process that keeps the people who read the original code apart from the people who write the new code) is the least risky known way to publish a compatible program.
It reduces legal risk, but it does not remove it. The section [Legal boundaries](#legal-boundaries) lists what remains open.

## Status of the earlier proofs

The earlier proofs are analysis work. Their ARM64 code follows Ghidra's reconstruction of the original functions.
Therefore, that code must never enter the shipped program, and the implementation side must never read it.
The proofs remain useful as private reference programs, because they show that the analysis describes the original behavior correctly.

- The [first native reconstruction experiment](native-first-proof.md) matches one function across 138,240 controlled inputs. A modified SC6 Blueprint asset also parses successfully.
- The [movement experiment](native-physics-proof.md) matches 147,456 movement steps across 18,432 constructed states for two connected functions. Rotation and battle lookup branches remain unsupported.
- The [Training-state comparison](native-live-proof.md) matches 128 captured fighter states across idle and movement captures. The game runs through FEX, and the comparison uses Rosetta 2 to execute the original instructions outside the game.
- The [recorder comparison](native-boundary-recorder.md) checks outputs from 64 actual movement calls inside SC6, with zero differences. Airborne movement and the unsupported branches remain unverified.

The recordings from the running game are behavior observations, not code.
The implementation side may use them as test data, under the rules in [Clean-room process](#clean-room-process).

## Active scope (2026-10-16)

The user deferred HDR, ProMotion, interpolation, and changes to game timing.
Those requirements remain future targets. They do not block the current work.
Metal rendering returned to scope on 2026-10-16. See [the Metal rendering plan](native-metal.md).
Preserve the original battle behavior and timing during this phase.

The immediate goal is a connected section of battle logic that is specified, rebuilt from the specification as new ARM64 code, and checked against recordings from the original game.
The user requires broad tests across sequences, rather than further isolated function probes.
The existing proofs do not meet that requirement. See [the broader test plan](native-battle-tests.md).

## Targets

- The minimum hardware is an M1 Pro.
- The minimum supported OS is macOS 27.0.
- The minimum requested converter is Metal Shader Converter 4.0 beta 2.
- The executable must contain native ARM64 code for the CPU.
- The renderer must use Metal.
- The shipped code must be new code written from specifications, plus libraries whose licenses permit distribution.
- The program must load game assets from original game files supplied by the user.
- The game must support HDR on XDR displays, with correct Display P3 color management.
- The game must support ProMotion, with rendering and visible animation updates targeting 120 Hz.
- The battle logic must advance at 60 Hz, independently of the display and rendering rates.

The converter version is a requirement for building or generating shaders. It is separate from the minimum supported OS.
Apple's public converter page currently names 4.0 beta, but it does not establish the exact beta 2 requirements.
Confirm those requirements from the installed package and its release notes before choosing GPU features.
Do not assume that every feature available on macOS 27.0 exists on an M1 Pro.

## Deferred display and timing requirements

The design below is retained for a later phase.
Do not implement it as part of the current battle-logic recovery work.

High dynamic range (HDR) allows highlights above the brightness used for ordinary white.
Display P3 defines a range of colors. Supporting P3 alone does not establish HDR support.
Apple's extended dynamic range (EDR) is the system that presents these brighter values through Metal.

The initial display design uses `CAMetalLayer` with `wantsExtendedDynamicRangeContent`, `extendedLinearDisplayP3`, and `RGBA16Float`.
This follows Apple's documented EDR approach. Confirm its suitability for the final renderer on the minimum hardware.

The renderer must preserve bright scene values until the final display conversion.
Tone mapping means fitting scene brightness into the range that the current display can show.
Its settings must follow the available display brightness, including changes during play.

1. Preserve scene brightness before an existing SDR conversion clips it.
2. Convert colors correctly into Display P3. Do not relabel existing colors as P3.
3. Keep UI brightness readable without forcing ordinary white to the display's maximum brightness.
4. Adapt the final image when the available HDR brightness changes.
5. Provide a correct standard dynamic range (SDR) result when HDR is unavailable.

HDR support requires visible scene highlights above ordinary white, with correct colors and retained detail.
Selecting a floating-point buffer alone does not satisfy this requirement.
Whether the shipped renderer exposes suitable scene values remains unverified.

ProMotion support requires scheduling frames with the display, rather than assuming every display interval lasts the same time.
Use `CAMetalDisplayLink` or a verified equivalent to coordinate rendering with presentation.
Request 120 Hz when supported, and handle lower display rates without changing battle speed.

Interpolation creates intermediate visible states between the states produced by battle steps.
The battle remains authoritative, so interpolated values must never change collision, damage, input windows, or animation events that affect gameplay.

1. Advance battle logic once per `1/60` second, which is about 16.667 milliseconds.
2. Preserve the visual states needed to draw between battle steps.
3. Draw character poses, positions, and cameras at up to 120 Hz, about 8.333 milliseconds between frames.
4. Use elapsed time for independent visual effects where their behavior permits it.
5. Execute required battle steps when rendering falls below 60 frames per second, subject to an explicit overload policy.
6. Reset interpolation across teleports, scene changes, and other discontinuities.

Do not merely display every 60 Hz pose twice. That would produce 120 presentations without smoother animation.
Blending previous and current completed states can delay visible responses by one battle step.
Measure that delay before choosing the final interpolation method. Do not claim that interpolation adds no delay without evidence.

Acceptance requires these checks on an M1 Pro:

- At 120 rendered frames per second, visible motion includes intermediate poses while battle logic completes 60 steps per second.
- At 90, 60, and 45 rendered frames per second, battle timing remains correct when the machine can sustain the required work.
- Replays and controlled input sequences produce matching battle results, using comparisons that account for the original floating-point behavior.
- On a compatible XDR display, highlights exceed ordinary white without incorrect P3 colors or lost highlight detail.
- Changes in display brightness and movement between HDR and SDR displays preserve a usable image.
- Frame presentation follows ProMotion without changing battle speed when the display changes its rate.
- Measurements report sustained rendering speed and input delay at recorded resolution and quality settings.

Supporting interpolation at 120 Hz does not establish sustained 120 frames per second on the minimum hardware.
The final performance settings and sustained result remain unverified.

## What was checked

- The current GhidrAssistMCP connection responds to requests.
- Its active project contains no files, and no program is open.
- An existing project marker is present at `re/ghidra/SC6.gpr`.
- The local `SoulcaliburVI.exe` exists and has 71,737,344 bytes.
- Existing project notes identify Unreal Engine 4.17.2 and Direct3D 11. These identifications were not independently repeated here.
- The workspace contains uncommitted work on the existing CrossOver mod.

An empty active project does not mean that the earlier analysis is lost.
Inspect the existing project before importing and analyzing another copy.

## Ghidra connection

Keep GhidrAssistMCP initially because the connection already works.
It exposes program selection, decompilation, references, types, and analysis jobs.

If a different server becomes necessary, prefer `bethington/ghidra-mcp` over `LaurieWired/GhidraMCP` for this investigation.
Its documentation describes operations on many functions at once and analysis without the UI.
These capabilities could help with a large executable. They have not been tested locally here.

A server exposes Ghidra operations. Changing servers does not recover missing source or make a native port automatic.

## Clean-room process

The process has two sides. Each side has fixed inputs and outputs.
The written record of that separation is part of the evidence, so keep it.

### Analysis side

The analysis side may use the original executable, Ghidra, `SC6-docs`, `re/`, the recorder, the private captures, and the earlier reconstructions.
Its only output to the implementation side is a specification (a written description of what a part of the game does, without the original code).

Each specification follows these rules:

- Describe inputs, outputs, state, rules, formulas, update order, and edge cases in prose, tables, or mathematics.
- Name state by its meaning, for example "horizontal velocity", not by an offset inside an original object.
- Do not include code, pseudocode, disassembly, or text copied from a decompiler.
- Do not include addresses or the memory layout of original game objects. The new code chooses its own layout.
- Include file-format details when the new program needs them to read the user's assets. Those details describe an interface, not the original program's code.
- Record each constant with the observation or measurement that supports it.
- Do not copy data tables, such as move data, into a specification. The new program loads them from the user's installation at runtime.

Before a specification goes to the implementation side, check it against these rules and record the check in a review log.
Store the log with the analysis notes.

### Implementation side

The implementation side writes new code from specifications only.

- It works in a separate repository that contains only specifications, new code, tests, and libraries with suitable licenses.
- It never opens the original executable in a disassembler, Ghidra, `re/`, `SC6-docs`, this repository's analysis notes, or the private investigation directory.
- An AI implementation session starts inside that separate repository, without the GhidrAssistMCP server and without paths to analysis material.
- Questions go back to the analysis side in writing. Answers come back as revised specifications, never as code.
- Tests may use private recordings from the original game: inputs, expected outputs, and the fields that differ. Recordings stay out of the shipped program.

This separation is weaker than two independent teams, because one person directs both sides.
Therefore, never paste analysis material into an implementation session, and keep the review log and session limits as the record.

## Release design

The public program is new code that runs against the user's installation.

1. The user selects the original installation.
2. The program checks cryptographic hashes, which identify the supported file contents.
3. The program rejects unsupported versions with a clear message.
4. The program loads assets from the installation at runtime.
5. Any converted data, such as a shader cache, is created locally and stays on the user's Mac.
6. Removing the installation prevents the program from running.

Checking hashes identifies files. It does not prove that the user owns them.
If stronger ownership verification is required, investigate a supported storefront mechanism and its terms separately.

The public package may contain:

- New code from the implementation side.
- Libraries whose licenses permit distribution, with their license texts.
- Specifications, but only after a separate publication review. Until then, keep them private.

Keep these materials outside public releases:

- Original executables, libraries, assets, movies, and audio.
- Decompiled game code, the earlier ARM64 reconstructions, and dumps from the analysis database.
- Recordings and captures from the game.
- Converted assets and shaders derived from the game.
- Decryption keys and other secrets.

Audit an actual release archive before publishing it.
Do not use the game's name or logos in a way that suggests that Bandai Namco made or approved the program.

## Legal boundaries

This section records risks and open questions. It is not legal advice.
Before any public release, a lawyer in the user's jurisdiction should review the program, the process record, and the release contents.

### Pak encryption (open, blocks release)

The index of `pakchunk0` is encrypted (pak version 4), so the program must decrypt it to load assets.

1. In the United States, 17 U.S.C. § 1201(a)(1) prohibits circumventing a measure that controls access to a copyrighted work.
2. Sections 1201(a)(2) and 1201(b) prohibit distributing tools or information that mainly serve that circumvention.
3. Therefore, the public program must never contain the AES key.
4. Reading the key from the user's own executable at runtime avoids distributing the key.
5. However, whether that decryption is lawful circumvention for the owner of a licensed copy is unsettled, I think.

Section 1201(f) permits some circumvention to achieve interoperability of an independently created program.
Whether it covers runtime decryption by this program is unverified.

The AES key exists in plain text in the exported Codex chat on the Desktop.
Do not share that file. Never write the key into Git, specifications, code, or tests.

### License terms (open)

The game's license and the Steam terms may prohibit reverse engineering. Nobody has checked them for this project.
United States courts have enforced such clauses, for example in *Bowers v. Baystate Technologies* (2003).
In the European Union, Article 6 of Directive 2009/24/EC permits decompilation for the interoperability of an independently created program, and a contract cannot remove that right.
That right is narrower than porting a game, so it may not cover this project.

### Supporting precedent

*Sega v. Accolade* (1992) and *Sony v. Connectix* (2000) held that copying during reverse engineering can be fair use.
In both cases, the copying was necessary to obtain functional elements that copyright does not protect, and the final product contained no copied code.
These are decisions of one United States appeals court. They support the clean-room shape, but they do not decide this project.

### Online play

The program must not connect to Bandai Namco's online services. Online play is out of scope.

## Technical decisions that remain open

Ghidra produces pseudocode, which is an approximation of source that helps explain a compiled program.
In the clean-room shape, pseudocode is analysis material only. It never becomes shipped code.

The new program must replace everything that the original executable does, not only battle logic:

- Read UE4 4.17 pak files and `.uasset`/`.uexp` assets from the user's installation.
- Run the game's Blueprint logic. Blueprints are bytecode stored in the assets, so the program needs a new interpreter or new code that matches their behavior.
- Provide animation, audio, input, menus, and rendering.

Each item needs a choice between writing new code and using an existing library.
Check each library's license before using it. Epic's Unreal Engine source has its own license terms. Verify those terms before considering any of that source for shipped code.
This is a large reimplementation, so the battle logic comes first, and the remaining systems need separate estimates.

The deferred shader path needs its own proof when rendering work resumes:

1. Inspect an actual game shader to identify its format.
2. If it uses DirectX Bytecode (DXBC), establish a conversion into DirectX Intermediate Language (DXIL).
3. Feed the DXIL into Metal Shader Converter to generate a Metal library.
4. Match the renderer's bindings, which connect shader inputs to textures, buffers, and samplers.
5. Compare the rendered result with the original on an M1 Pro.

Apple documents DXIL input for Metal Shader Converter. Do not assume it directly accepts every Direct3D 11 shader.
Converting shaders does not replace the renderer that submits drawing commands.

## First clean-room proof

The first proof tests the process on one connected battle section, not on one isolated function.

1. The analysis side chooses the section from [the broader test plan](native-battle-tests.md), starting from the main character update.
2. The analysis side writes the specification and records its review.
3. Create the separate implementation repository. Put in only the specification, the test interface, and licensed libraries.
4. An implementation session writes new ARM64 code from the specification.
5. Run the new code on broad recorded sequences from the original game for both fighters.
6. Report the first differing update, the affected fields, and the scenario.
7. Send each difference back to the analysis side as a written question. The analysis side answers with a revised specification.
8. Inspect the implementation repository to confirm that it contains no original material.

This proof establishes that the process can rebuild one connected section with matching behavior.
It does not establish asset loading, a playable game, a working renderer, or legal clearance.

The following proofs belong to the deferred phase:

- Separate battle advancement from visible animation updates in the new code.
- Present an independently generated HDR scene through Metal on an XDR display while following ProMotion.

The independent scene checks the display code. It does not prove that the game's assets can supply correct HDR values.

## Sources

- [GhidrAssistMCP](https://github.com/symgraph/GhidrAssistMCP)
- [bethington/ghidra-mcp](https://github.com/bethington/ghidra-mcp)
- [LaurieWired/GhidraMCP](https://github.com/lauriewired/ghidramcp)
- [Apple: Metal shader converter](https://developer.apple.com/metal/shader-converter/)
- [Microsoft: DXIL specification](https://github.com/Microsoft/DirectXShaderCompiler/blob/main/docs/DXIL.rst)
- [Apple: Explore HDR rendering with EDR](https://developer.apple.com/videos/play/wwdc2021/10161/)
- [Apple: CAMetalDisplayLink preferredFrameRateRange](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/preferredframeraterange)
- [17 U.S.C. § 1201](https://www.law.cornell.edu/uscode/text/17/1201)
- Directive 2009/24/EC of the European Parliament and of the Council, Article 6.
- *Sega Enterprises v. Accolade*, 977 F.2d 1510 (9th Cir. 1992).
- *Sony Computer Entertainment v. Connectix*, 203 F.3d 596 (9th Cir. 2000).
- *Bowers v. Baystate Technologies*, 320 F.3d 1317 (Fed. Cir. 2003).
