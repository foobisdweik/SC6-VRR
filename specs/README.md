# Specifications for the clean-room implementation

This folder holds the specifications that the analysis side writes for the implementation side. The process is described in [native-port.md](../notes/native-port.md#clean-room-process).

Each specification describes behavior in its own words. It names what a part of the game does and what the new code must reproduce.

## Rules for every specification

- Describe inputs, outputs, state, order, sizes, formats, and edge cases in prose, tables, or mathematics.
- Name things by their role, for example "scene depth", not by an address or an object inside the original program.
- Do not include code, pseudocode, disassembly, or text copied from a decompiler.
- Do not include memory addresses or the memory layout of original game objects.
- Mark each statement as **observed** (measured in a capture) or **inferred** (an interpretation of observations).
- Name the captures that support each observation. The captures themselves stay private.

## Status of a specification

1. **Draft:** written by the analysis side, not yet reviewed.
2. **Reviewed:** checked against the rules above, with the check recorded in [REVIEW.md](REVIEW.md).
3. **Released:** copied to the implementation repository. Only reviewed specifications are released.

## Index

| ID | Title | Status |
| --- | --- | --- |
| R-001 | [Renderer frame structure](renderer-frame-structure.md) | Draft 2, reviewed (rules check only) |
