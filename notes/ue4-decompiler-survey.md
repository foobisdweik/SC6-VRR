# UE4 decompiler survey

Researched on 2026-10-16 using Exa Search and original project documentation.
The search requested 44 results across eight queries covering native code, Blueprint recovery, and engine inspection tools.
Results overlap, so 44 results does not mean 44 distinct projects.

Useful recovery tools exist, but this search did not identify a demonstrated complete decompiler for SC6's native executable.
Knowing the engine version helps choose tools and interpret structures. It does not restore the game's missing C++ source.

## What qualifies as evidence

- A project's documented support range is a compatibility claim, not a successful SC6 test.
- Test code establishes what the project checks. It does not establish that those tests pass on SC6.
- A published test report records one set of results. It does not guarantee success for every asset.
- SC6-specific documentation is stronger evidence for this game, but its accuracy still needs verification against our executable.

No tool was installed, injected, or run against the game during this survey.

Follow-up on 2026-10-16: the user supplied `kismet-analyzer-e8982e9-osx-x64/` in the workspace.
Its downloaded archive matched the SHA-256 digest published by GitHub for the release.
The extracted executable matched the archive. Its file type is a Mac Intel executable.
After restoring its executable permission, `--help` completed with exit code 0 on this Mac.
Its help lists `VER_UE4_17`. No SC6 asset has been parsed during this check.
Startup succeeded without installing another .NET runtime, so the earlier runtime concern is not a blocker for this downloaded build.
Graph generation and ARM64 compilation remain unverified.

A later [local experiment](native-first-proof.md) successfully parsed a UE4.17.2 template asset and a modified SC6 widget.
The widget's JSON contains four Blueprint function entries with script instructions.
This establishes limited SC6 format support for the supplied Kismet Analyzer build.
It does not establish complete decompilation or compilation back into assets.

## Candidates

| Project | What it recovers or provides | Evidence and limits |
| --- | --- | --- |
| [KismetKompiler](https://github.com/tge-was-taken/KismetKompiler) | It decompiles Blueprint scripts into readable `.kms` text and compiles that text back into assets. | Its README names tests on Shin Megami Tensei V assets using UE4.23. Its test source explicitly uses UE4.18 for Dragon Quest XI S. SC6 and UE4.17 were not verified. |
| [UeBlueprintDumper](https://github.com/CrystalFerrai/UeBlueprintDumper) | It exports Blueprint properties, function information, and disassembled script instructions into JSON. | Its documented engine list includes `UE4_17`. That is a support claim, not an SC6 result. It does not recover native C++ implementations. |
| [Dumper-7](https://github.com/Encryqed/Dumper-7) | It generates a software development kit (SDK), which describes game types and callable functions for other code. | Its README claims all UE4 and UE5 versions. It documents object layouts for UE4.11 through UE4.20. SC6 behavior and operation through CrossOver remain unverified. |
| [UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) | It inspects objects while the game runs and exports headers that describe classes and properties. | Its documented target range includes UE4.17. Its authors explicitly say some games need adjustments. Generated headers do not contain the original native function implementations. |
| [UAssetAPI](https://github.com/atenfyr/UAssetAPI) | It reads and writes packaged assets, including Blueprint instructions. | Its documented range starts around UE4.13, which includes UE4.17. It is a supporting library, not a native executable decompiler. |
| [kismet-analyzer](https://github.com/trumank/kismet-analyzer) | It analyzes Blueprint instructions and draws graphs showing possible paths through the script. | Its README includes an asset-analysis example. It does not establish SC6 or UE4.17 compatibility. |
| [UE4GameProjectGenerator](https://github.com/Buckminsterfullerene02/UE4GameProjectGenerator) | It generates an editor project from exported headers and project descriptions. | Its required inputs include a UE4SS header dump and matching engine tools. Generating the project does not establish recovery of original native function bodies. |

Blueprints are UE4's scripted logic stored in game assets.
Native C++ code is compiled into machine instructions in the executable or libraries.
Recovering the former does not recover the latter.

## Host platform limits

The first survey did not adequately check whether the recommended tools run on macOS.
Engine compatibility and host compatibility are separate requirements.

[KismetKompiler's executable project](https://github.com/tge-was-taken/KismetKompiler/blob/main/src/KismetKompiler/KismetKompiler.csproj) targets `.NET 8` and declares `win-x64` as its runtime.
Its [release workflow](https://github.com/tge-was-taken/KismetKompiler/blob/main/.github/workflows/dotnet.yml) publishes on Windows for Windows.
The library targets `.NET 8` without a Windows-specific framework name.
Therefore, a native Mac build is worth investigating, but this inspection does not establish that its dependencies or execution work on macOS.

[UeBlueprintDumper's executable project](https://github.com/CrystalFerrai/UeBlueprintDumper/blob/main/UeBlueprintDumper/UeBlueprintDumper.csproj) targets `.NET 8` and declares an x64 platform.
Its build explicitly copies `oo2core_9_win64.dll` and `zlib-ng2.dll` into the output.
Those are Windows native libraries. A native Mac port needs compatible replacements or verified paths that do not require them.
Changing the output target alone does not resolve these dependencies.

Neither tool is a verified native macOS recommendation.
Running their Windows builds through CrossOver would be a separate experiment, not proof of native Mac compatibility.
Use the existing Mac installation of Ghidra for native executable analysis while evaluating these candidates separately.

## Strongest Blueprint candidate: KismetKompiler

The project provides an actual decompiler and compiler, rather than only an asset viewer.
Its [0.4.0-alpha release notes](https://github.com/tge-was-taken/KismetKompiler/releases/tag/0.4.0-alpha) report fixes for versions below UE4.23.
Those notes describe additional tests for older games.

The [Dragon Quest XI S test source](https://github.com/tge-was-taken/KismetKompiler/blob/main/src/KismetKompiler.Tests/Recompilation/DQXIS.Generated.cs) selects `VER_UE4_18`.
That is evidence for testing an adjacent engine version. It is not evidence for successful UE4.17 or SC6 recovery.

The [test implementation](https://github.com/tge-was-taken/KismetKompiler/blob/main/src/KismetKompiler.Tests/Recompilation/RecompilationTestsBase.cs) performs these steps:

1. Read an asset and check that the asset library can reproduce its original bytes.
2. Decompile its Blueprint functions into text.
3. Compile that text and insert the resulting script into an asset.
4. Compare serialized representations of the original and regenerated function instructions.

The final comparison checks function instructions. It does not compare every byte of the regenerated package or run the game.
The published [compatibility report](https://github.com/tge-was-taken/KismetKompiler/blob/main/Compatibility.csv) contains passing and failing results.
The README states that some Blueprint constructs remain unsupported.

## Existing evidence for native SC6 analysis

[SC6ModdingDocs](https://github.com/FottenSC/SC6ModdingDocs) describes Ghidra analysis of the shipping Steam executable.
Its README says that recovered class layouts, addresses, and functions were checked with probes in the running game.
The workspace already contains this project as the `SC6-docs` submodule.

[HorseMod](https://github.com/FottenSC/HorseMod) is an SC6 development environment with replay inspection features.
It provides relevant game-specific work, but it is not a complete decompiled source tree or a native Mac port.

This evidence supports retaining Ghidra for native functions and enriching its analysis with recovered engine information.
Neither project demonstrates automatic recovery of a complete rebuildable SC6 C++ project.

## Candidates outside the target version

[Spuckwaffel/UEDumper](https://github.com/Spuckwaffel/UEDumper) starts its documented support at UE4.19.
[McDaived/UE-Dumper](https://github.com/McDaived/UE-Dumper) starts its documented UE4 range at UE4.20.
Neither is a first choice for UE4.17 without adaptation.

## Recommended order for local evaluation

1. Use the existing SC6 Ghidra analysis and verify its executable identity.
2. Test the supplied Mac Intel build of Kismet Analyzer on a small SC6 asset using `VER_UE4_17`.
3. Establish a native Mac build of KismetKompiler before treating it as part of the Mac workflow.
4. If that succeeds, test it on a small SC6 Blueprint asset with verification enabled.
5. Evaluate UeBlueprintDumper separately because it has explicit Windows native dependencies.
6. Use Dumper-7 or UE4SS through a separately verified Windows environment when additional information is needed for native analysis.
7. Compare recovered names and fields with the existing SC6 documentation before importing them into Ghidra.

Keep extracted assets, generated headers, and decompiled output private under the port plan's release boundaries.

The search strengthens the case for selective decompilation now.
It does not establish that a complete source port or a complete static recompilation is practical.
The recommendation to investigate recompilation remains provisional until a suitable x64 translator is demonstrated on SC6 functions.
